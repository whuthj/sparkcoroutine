#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include "sparkcoroutine\coroutine.hpp"

using namespace Spark::Thread;

bool g_shutdown;
std::vector<int> g_vecTask;

//生产者
void do_producer()
{
    const int RUN_TIMES = 100;
    g_shutdown = false;

    for (int i = 1; i <= RUN_TIMES; i++)
    {
        printf("生产第%d个任务\r\n", i);
        g_vecTask.push_back(i);

        //通知消费者
        Coroutine::yield();
    }

    g_shutdown = true;
}

//消费者
void do_consumer()
{
    while (!g_shutdown)
    {
        if (g_vecTask.size() == 0)
        {
            //没有任务通知生产者
            Coroutine::yield();
            continue;
        }

        printf("消费第%d个任务\r\n", g_vecTask.back());
        g_vecTask.pop_back();

        //任务执行完成通知生产者再生产
        Coroutine::yield();
    }
}

/**
 *  实现生产者-消费者模型
 */
int main(int argc, char* argv[])
{
    Coroutine::init();
    std::shared_ptr<void> auto_uinitialize(nullptr, [](void*){ Coroutine::uninit(); });

    Coroutine consumer([]() { do_consumer(); });
    Coroutine producer([]() { do_producer(); });

    Coroutine::manage(consumer);
    Coroutine::manage(producer);

    while (!g_shutdown) {
        Coroutine::yield();
    };

    return 0;
}

