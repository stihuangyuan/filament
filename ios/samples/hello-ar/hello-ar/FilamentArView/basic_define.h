//
//  basic_define.h
//  hello-ar
//
//  Created by user on 7/2/22.
//

#ifndef basic_define_h
#define basic_define_h

#include <vector>

#if 1
#define TS(name) std::chrono::time_point<std::chrono::system_clock> t_start_##name = std::chrono::system_clock::now()
#define TE(name) 1000 * (std::chrono::duration<double>(std::chrono::system_clock::now() - t_start_##name)).count()
#else
#define TS(name)
#define TE(name)
#endif

template <int N> struct Data50
{
    Data50()
    {
        count = 0;
        valid_count = 0;
        average = 0;
        memset(data, 0, sizeof(float) * N);
    }

    float addData(float new_data)
    {
        data[count] = new_data;
        count++;
        if (count == N)
        {
            count = 0;
        }

        float sum = 0;
        for (int i = 0; i < N; i++)
        {
            sum += data[i];
        }
        return sum / (float)N;
    }

    int count, valid_count;
    float average;
    float data[N];
};

#endif /* basic_define_h */
