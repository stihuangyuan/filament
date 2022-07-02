/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#import "FilamentArViewController.h"

#import "FilamentApp.h"

#import "MathHelpers.h"

#import <ARKit/ARKit.h>

#import "basic_define.h"

#define METAL_AVAILABLE __has_include(<QuartzCore/CAMetalLayer.h>)

#if !METAL_AVAILABLE
#error The iOS simulator does not support Metal.
#endif

@interface FilamentArViewController () <ARSessionDelegate> {
    FilamentApp* app;
}

@property (nonatomic, strong) ARSession* session;

// The most recent anchor placed via a tap on the screen.
@property (nonatomic, strong) ARAnchor* anchor;

@property (nonatomic, strong) UILabel *accInfoLabel;

@end

@implementation FilamentArViewController

- (UILabel *)accInfoLabel
{
    if (!_accInfoLabel) {
        CGRect frame = CGRectMake(self.view.bounds.size.width - 250, 0, 400, 290);
        _accInfoLabel = [[UILabel alloc] initWithFrame:frame];
        _accInfoLabel.textAlignment = NSTextAlignmentLeft;
        _accInfoLabel.numberOfLines = 0;
        _accInfoLabel.textColor = [UIColor redColor];
    }
    return _accInfoLabel;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    CGRect nativeBounds = [[UIScreen mainScreen] nativeBounds];
    uint32_t nativeWidth = (uint32_t) nativeBounds.size.width;
    uint32_t nativeHeight = (uint32_t) nativeBounds.size.height;
#if FILAMENT_APP_USE_OPENGL
    // Flip width and height; OpenGL layers are oriented "sideways"
    const uint32_t tmp = nativeWidth;
    nativeWidth = nativeHeight;
    nativeHeight = tmp;
#endif

    [self.view addSubview:self.accInfoLabel];

    NSString* resourcePath = [NSBundle mainBundle].bundlePath;
    app = new FilamentApp((__bridge void*) self.view.layer, nativeWidth, nativeHeight, 
        utils::Path([resourcePath cStringUsingEncoding:NSUTF8StringEncoding]));

    self.session = [ARSession new];
    self.session.delegate = self;

    UITapGestureRecognizer* tapRecognizer =
            [[UITapGestureRecognizer alloc] initWithTarget:self
                                                    action:@selector(handleTap:)];

    tapRecognizer.numberOfTapsRequired = 1;
    [self.view addGestureRecognizer:tapRecognizer];

    UIPinchGestureRecognizer *pinchGestureRecognizer =
            [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
    [self.view addGestureRecognizer:pinchGestureRecognizer];
}

- (void)viewWillAppear:(BOOL)animated
{
    [super viewWillAppear:animated];

    ARWorldTrackingConfiguration* configuration = [ARWorldTrackingConfiguration new];
    configuration.planeDetection = ARPlaneDetectionHorizontal;
    [self.session runWithConfiguration:configuration];
}

- (void)viewWillDisappear:(BOOL)animated
{
    [super viewWillDisappear:animated];

    [self.session pause];
}

- (void)dealloc
{
    delete app;
}

#pragma mark ARSessionDelegate

#define use_portrait 1

void printf_matrix44(const simd_float4x4& mat, const std::string& header)
{
    printf("%s:\n%.6f %.6f %.6f %.6f\n%.6f %.6f %.6f %.6f\n%.6f %.6f %.6f %.6f\n%.6f %.6f %.6f %.6f\n",
        header.c_str(), 
        mat.columns[0][0], mat.columns[1][0], mat.columns[2][0], mat.columns[3][0],
        mat.columns[0][1], mat.columns[1][1], mat.columns[2][1], mat.columns[3][1],
        mat.columns[0][2], mat.columns[1][2], mat.columns[2][2], mat.columns[3][2],
        mat.columns[0][3], mat.columns[1][3], mat.columns[2][3], mat.columns[3][3]
    );
}

struct Data50<30> fps_data50, algo_data50, render_data50;
float avg_algo_cost, avg_render_cost, avg_fps;
float px, py, pz;

double last_img_time = 0;
- (void)session:(ARSession *)session didUpdateFrame:(ARFrame *)frame
{
    double curr_img_time = frame.timestamp;
    
    // The height and width are flipped for the viewport because we're requesting transforms in the
    // UIInterfaceOrientationLandscapeRight orientation (landscape, home button on the right-hand
    // side).
    CGRect nativeBounds = [[UIScreen mainScreen] nativeBounds];
#if use_portrait
    CGSize viewport = CGSizeMake(nativeBounds.size.width, nativeBounds.size.height);
#else
    CGSize viewport = CGSizeMake(nativeBounds.size.height, nativeBounds.size.width);
#endif

    // This transform gets applied to the UV coordinates of the full-screen triangle used to render
    // the camera feed. We want the inverse because we're applying the transform to the UV
    // coordinates, not the image itself.
    // (See camera_feed.mat and FullScreenTriangle.cpp)
    CGAffineTransform displayTransform =
#if use_portrait
            [frame displayTransformForOrientation:UIInterfaceOrientationPortrait
#else
            [frame displayTransformForOrientation:UIInterfaceOrientationLandscapeRight
#endif
                                     viewportSize:viewport];
    CGAffineTransform transformInv = CGAffineTransformInvert(displayTransform);
    mat3f textureTransform(transformInv.a, transformInv.b, 0,
                           transformInv.c, transformInv.d, 0,
                           transformInv.tx, transformInv.ty, 1);

    const auto& projection =
#if use_portrait
            [frame.camera projectionMatrixForOrientation:UIInterfaceOrientationPortrait
#else
            [frame.camera projectionMatrixForOrientation:UIInterfaceOrientationLandscapeRight
#endif
                                            viewportSize:viewport
                                                   zNear: 0.01f
                                                    zFar:10.00f];

#if use_portrait
    auto viewMatrix = [frame.camera viewMatrixForOrientation:UIInterfaceOrientationPortrait];
    auto cameraTransformMatrix = simd_inverse(viewMatrix);
#endif

    const auto& transform = frame.camera.transform;
    px = transform.columns[3][0], py = transform.columns[3][1], pz = transform.columns[3][2]; 
    // printf_matrix44(frame.camera.transform, "camera transform");

    TS(t_render);
    // frame.camera.transform gives a camera transform matrix assuming a landscape-right orientation.
    // For simplicity, the app's orientation is locked to UIInterfaceOrientationLandscapeRight.
    app->render(FilamentApp::FilamentArFrame {
        .cameraImage = (void*) frame.capturedImage,
        .cameraTextureTransform = textureTransform,
        .projection = FILAMENT_MAT4_FROM_SIMD(projection),
#if use_portrait
        .view = FILAMENT_MAT4F_FROM_SIMD(cameraTransformMatrix)
#else
        .view = FILAMENT_MAT4F_FROM_SIMD(frame.camera.transform)
#endif
    });
    float render_cost = TE(t_render);
    avg_render_cost = render_data50.addData(render_cost);

    float time_diff = curr_img_time - last_img_time;
    float fps = 1.0 / time_diff;
    avg_fps = fps_data50.addData(fps);
    last_img_time = curr_img_time;

    NSString *stemp = [NSString stringWithFormat:@"FPS %.2f\n\n算法耗时 %.2f ms\n\n渲染耗时 %.2f ms\n\np: %.3f %.3f %.3f", 
            avg_fps, avg_algo_cost, avg_render_cost, 
            px, py, pz];
    _accInfoLabel.text = stemp;
}

- (void)handleTap:(UITapGestureRecognizer*)sender
{
    if (self.anchor) {
        [self.session removeAnchor:self.anchor];
    }

    ARFrame* currentFrame = self.session.currentFrame;
    if (!currentFrame) {
        return;
    }

    CGPoint point = [sender locationInView:self.view];
    double u = point.x / self.view.bounds.size.width;
    double v = point.y / self.view.bounds.size.height;
    
    int bufferWidth = (int)CVPixelBufferGetWidthOfPlane(currentFrame.capturedImage,0);
    int bufferHeight = (int)CVPixelBufferGetHeightOfPlane(currentFrame.capturedImage, 0);
    int bytePerRow = (int)CVPixelBufferGetBytesPerRowOfPlane(currentFrame.capturedImage, 0);
    // printf("arkit byte image size: %d %d %d\n", bufferWidth, bufferHeight, bytePerRow);
    CGPoint image_point;
    /* image_point.x = v * bufferWidth;
    image_point.y = (1-u) * bufferHeight; */
    image_point.x = v;
    image_point.y = (1-u);

    // printf("view point %.3f %.3f, image point %.3f %.3f\n", point.x, point.y, image_point.x, image_point.y);
    
    NSArray<ARHitTestResult *> *result = [currentFrame hitTest:image_point types:ARHitTestResultTypeExistingPlaneUsingExtent];
    if (result.count == 0) {
        printf("no hit result???\n");
        return;
    }
    printf("hit result num %d\n", result.count);

    // If there are multiple hits, just pick the closest plane
    ARHitTestResult * hitResult = [result firstObject];
    mat4f objectTransform = FILAMENT_MAT4F_FROM_SIMD(hitResult.worldTransform);
    
    // Create a transform 0.2 meters in front of the camera.
    /* mat4f viewTransform = FILAMENT_MAT4F_FROM_SIMD(currentFrame.camera.transform);
    mat4f objectTranslation = mat4f::translation(float3{0.f, 0.f, -.2f});
    mat4f objectTransform = viewTransform * objectTranslation; */

    app->setObjectTransform(objectTransform);

   simd_float4x4 simd_transform = SIMD_FLOAT4X4_FROM_FILAMENT(objectTransform);
   self.anchor = [[ARAnchor alloc] initWithName:@"object" transform:simd_transform];
   [self.session addAnchor:self.anchor];
}

- (void) handlePinch:(UIPinchGestureRecognizer*) recognizer
{
    if (recognizer.state ==UIGestureRecognizerStateChanged &&
        recognizer.numberOfTouches == 2)
    {
        app->setObjectScale((recognizer.scale - 1.0) / 50 + 1.0);
    }
}

- (void)session:(ARSession *)session didUpdateAnchors:(NSArray<ARAnchor*>*)anchors
{
    for (ARAnchor* anchor : anchors) {
        if ([anchor isKindOfClass:[ARPlaneAnchor class]]) {
            ARPlaneAnchor* planeAnchor = (ARPlaneAnchor*) anchor;
            const auto& geometry = planeAnchor.geometry;
            app->updatePlaneGeometry(FilamentApp::FilamentArPlaneGeometry {
                .transform = FILAMENT_MAT4F_FROM_SIMD(planeAnchor.transform),
                // geometry.vertices is an array of simd_float3's, but they're padded to be the
                // same length as a float4.
                .vertices = (float4*) geometry.vertices,
                .indices = (uint16_t*) geometry.triangleIndices,
                .vertexCount = geometry.vertexCount,
                .indexCount = geometry.triangleCount * 3
            });
            continue;
        }

        filament::math::mat4f transform = FILAMENT_MAT4F_FROM_SIMD(anchor.transform);
        app->setObjectTransform(transform);
    }
}

@end
