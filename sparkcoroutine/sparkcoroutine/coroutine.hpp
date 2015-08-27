#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <functional>
#include <windows.h>

#ifndef _SPARK_ASSERT
#include <assert.h>
#define _SPARK_ASSERT assert
#endif

namespace Spark 
{ 
    namespace Thread 
    { 
        namespace _Detail 
        {
            class CoroutineImpl;
            class CoroutineMgr;
        }

        class Coroutine
        {
        public:
            enum class Status
            {
                Supended,    // 挂起状态 （不需要运行状态，因为单线程调用，外部调用者测试协程状态时该协程一定处于挂起或者某种结束状态中）
                Finished,    // 执行结束
                Excepted,    // 发生了异常，同时也表示结束
            };

        public:
            //在当前线程初始化协程运行环境（转换当前线程为主协程）
            static void init();

            //清理当前线程的协程运行环境（转换协程为线程，必须在 init 方法得到的主协程中执行）
            static void uninit();

            // 把一个协程交给主协程托管，由主协程调度执行
            static void manage(Coroutine& fiber);

            //阻塞当前协程，控制权交给父协程（调用该协程的协程），如果是主协程调用，则阻塞主线程，控制权转移至托管协程（实际是遍历执行托管协程）
            static void yield();

            //阻塞当前协程，控制权交给父协程（调用该协程的协程），直到满足条件才能恢复执行，若主协程调用
            //则循环遍历执行托管线程直到条件满足
            template<class _Fn>
            static void yield_until(_Fn condition);

            //等待传入协程运行结束，等待过程阻塞当前协程，控制权交给当前协程的父协程（调用该协程的协程）；
            //主协程一般要实现线程消息队列，所以拿到控制权后可以和消息队列一起轮询执行，不会阻塞线程；如果
            //在主协程中直接调用 await 方法则导致线程阻塞，故不建议在主线程使用.
            static void await(Coroutine& fiber);

            //等待所有传入协程运行结束，行为和 await 方法相当
            template<class _Array>
            static void await_all(_Array& fibers);

            //等待所有传入协程运行结束，行为和 await 方法相当
            template<class _Iterator>
            static void await_all(_Iterator& first, _Iterator& last);

        public:
            explicit Coroutine()
            {
            }

            Coroutine(Coroutine&& other_) 
            {
                _impl.swap(other_._impl);
            }

            explicit Coroutine(const std::function<void(void)>& task);

            explicit Coroutine(const Coroutine&) = delete;

            Coroutine& operator=(Coroutine&& other_)
            {
                if (_impl)
                    terminate();
                _impl.swap(other_._impl);

                return (*this);
            }
    
            Coroutine& operator=(const Coroutine&) = delete;

            //判断协程是否有效（存在）
            operator bool() const
            {
                return (bool)_impl;
            }

            //重置 Coroutine 对象，若该协程处于 Supended 状态则抛出异常，非 Supended 状态则删除协程并置空，不包含
            //协程则直接返回
            void reset();

            //恢复执行该协程，当前状态必须为 Status::Supended，只能由其他协程调用
            void resume();

            //查询协程状态，状态为 Status::Supended 时可以调用 resume 方法但不可释放，其他状态时可释放该协程；
            Status status() const;

            void go(const std::function<void(void)>& task);

        protected:
            explicit Coroutine(const std::shared_ptr<_Detail::CoroutineImpl>& impl)
                : _impl(impl)
            {
            }

            const std::shared_ptr<_Detail::CoroutineImpl>& get_impl() const
            {
                return _impl;
            }

        private:
            std::shared_ptr<_Detail::CoroutineImpl>        _impl;
        };

        namespace _Detail 
        {
            struct CoroutineData
            {
                CoroutineMgr * const        major_fiber;
                CoroutineImpl * const        current_fiber;
            };

            inline CoroutineMgr* _get_major_fiber()
            {
                CoroutineData* data = (CoroutineData*)::GetFiberData();
                return data->major_fiber;
            }

            inline CoroutineImpl* _get_current_fiber()
            {
                CoroutineData* data = (CoroutineData*)::GetFiberData();
                return data->current_fiber;
            }

            class CoroutineMgr
            {
            public:
                explicit CoroutineMgr()
                    : _thread_id(::GetCurrentThreadId())
                {
                    _SPARK_ASSERT(!::IsThreadAFiber());
                    uint32_t inital_thread_id = _thread_id;

                    _data = std::make_shared<CoroutineData>(CoroutineData{ this, nullptr });
                    _handle = std::shared_ptr<void>(::ConvertThreadToFiberEx(_data.get(), FIBER_FLAG_FLOAT_SWITCH), [inital_thread_id](void* fib)
                    {
                        if (fib)
                        {
                            _SPARK_ASSERT(::GetCurrentThreadId() == inital_thread_id);
                            _SPARK_ASSERT(::GetCurrentFiber() == fib);
                            ::ConvertFiberToThread();
                        }
                    });

                    if (!_handle)
                        throw std::exception("ConvertThreadToFiberEx failed");
                }

                void manage(Coroutine& fiber)
                {
                    _fiber_list.push_back(std::move(fiber));
                }

                void yield()
                {
                    for (auto it = _fiber_list.begin(), end = _fiber_list.end(); it != end;)
                    {
                        Coroutine& fib = *it;

                        if (fib.status() == Coroutine::Status::Supended)
                            fib.resume();

                        if (fib.status() != Coroutine::Status::Supended)
                            it = _fiber_list.erase(it);
                        else
                            ++it;
                    }
                }

                template<class _Fn>
                void yield_until(_Fn condition)
                {
                    while (!condition())
                    {
                        yield();
                    }
                }

                uint32_t get_thread_id() const
                {
                    return _thread_id;
                }

            private:
                std::shared_ptr<CoroutineData>      _data;
                std::shared_ptr<void>               _handle;
                const uint32_t                      _thread_id;
                std::list<Coroutine>                _fiber_list;
            };

            class CoroutineImpl
            {
            public:
                explicit CoroutineImpl(const std::function<void(void)>& task)
                    : _task(task)
                {
                    CoroutineMgr* root_fiber = _get_major_fiber();
                    _SPARK_ASSERT(root_fiber && ::GetCurrentThreadId() == root_fiber->get_thread_id());

                    _data = std::make_shared<CoroutineData>(CoroutineData{ root_fiber, this });
                    _handle = std::shared_ptr<void>(::CreateFiberEx(0, 0, FIBER_FLAG_FLOAT_SWITCH, &CoroutineImpl::static_fiber_proc, _data.get()), [](void* fib)
                    {  
                        _SPARK_ASSERT(_get_major_fiber() && _get_major_fiber()->get_thread_id() == ::GetCurrentThreadId());
                        ::DeleteFiber(fib); 
                    });

                    if (!_handle)
                        throw std::exception("CreateFiberEx failed");
                }

                virtual ~CoroutineImpl()
                {
                    _SPARK_ASSERT(_status != Coroutine::Status::Supended);
                    _SPARK_ASSERT(_get_current_fiber() != this);
                }

                void resume()
                {
                    _SPARK_ASSERT(::GetCurrentThreadId() == _get_major_fiber()->get_thread_id());
                    _SPARK_ASSERT(_get_current_fiber() != this);
                    _SPARK_ASSERT(_status == Coroutine::Status::Supended);

                    if (_resume_condition())
                    {
                        _fiber_come_from = ::GetCurrentFiber();
                        ::SwitchToFiber(_handle.get());
                    }
                }

                void yield()
                {
                    _SPARK_ASSERT(_get_current_fiber() == this);
                    yield_until(always_true);
                }

                template<class _Fn>
                void yield_until(_Fn condition)
                {
                    _SPARK_ASSERT(_get_current_fiber() == this);
                    _SPARK_ASSERT(_fiber_come_from != nullptr);

                    _resume_condition = condition;
                    ::SwitchToFiber(_fiber_come_from);
                }

                Coroutine::Status get_status() const
                {
                    _SPARK_ASSERT(::GetCurrentThreadId() == _get_major_fiber()->get_thread_id());
                    _SPARK_ASSERT(_get_current_fiber() != this);
                    return _status;
                }

            private:
                static void CALLBACK static_fiber_proc(void* param)
                {
                    CoroutineData* data = (CoroutineData*)param;
                    _SPARK_ASSERT(data != nullptr && data->current_fiber != nullptr);

                    data->current_fiber->do_fiber_work();
                }

                void do_fiber_work()
                {
                    try
                    {
                        _task();
                        _status = Coroutine::Status::Finished;
                    }
                    catch (...)
                    {
                        _status = Coroutine::Status::Excepted;
                    }

                    yield();
                }

                static bool always_true()
                {
                    return true;
                }

            private:
                std::shared_ptr<CoroutineData>      _data;
                std::shared_ptr<void>               _handle;
                std::function<void(void)>           _task;
                std::function<bool(void)>           _resume_condition = always_true;
                Coroutine::Status                   _status = Coroutine::Status::Supended;
                void*                               _fiber_come_from = nullptr;
            };
        }

        void Coroutine::init()
        {
            _Detail::CoroutineMgr* major_fiber = new _Detail::CoroutineMgr();
            _SPARK_ASSERT(_Detail::_get_major_fiber() == major_fiber);
        }

        void Coroutine::uninit()
        {
            _Detail::CoroutineData* data = (_Detail::CoroutineData*)::GetFiberData();
            _SPARK_ASSERT(data->major_fiber != nullptr && data->current_fiber == nullptr);

            delete data->major_fiber;
        }

        void Coroutine::manage(Coroutine& fiber)
        {
            _Detail::CoroutineMgr* major_fiber = _Detail::_get_major_fiber();
            _SPARK_ASSERT(major_fiber != nullptr);

            major_fiber->manage(fiber);
        }

        template<class _Array>
        void Coroutine::await_all(_Array& array_)
        {
            await_all(array_.begin(), array_.end());
        }

        template<class _Iterator>
        void Coroutine::await_all(_Iterator& first, _Iterator& last)
        {
            _Detail::CoroutineData* data = (_Detail::CoroutineData*)::GetFiberData();

            bool all_done = false;

            while (!all_done)
            {
                all_done = true;

                for (auto it = first; it != last; ++it)
                {
                    Coroutine& fib = *it;
                    _SPARK_ASSERT(data->current_fiber != fib._impl.get());

                    if (fib.status() == Coroutine::Status::Supended)
                    {
                        fib.resume();

                        if (fib.status() == Coroutine::Status::Supended)
                            all_done = false;
                    }
                }

                if (!all_done)
                {
                    if (data->current_fiber)
                        Coroutine::yield();
                    else
                        ::Sleep(1);
                }
            }
        }

        inline Coroutine::Coroutine(const std::function<void(void)>& task)
            : _impl(std::make_shared<_Detail::CoroutineImpl>(task))
        {
        }

        inline void Coroutine::go(const std::function<void(void)>& task)
        {
            if (!_impl)
                _impl = std::make_shared<_Detail::CoroutineImpl>(task);
        }

        inline void Coroutine::reset()
        {
            if (_impl && _impl->get_status() == Status::Supended)
                throw std::exception("can't reset");
            _impl.reset();
        }

        inline void Coroutine::resume()
        {
            _SPARK_ASSERT(_impl);
            return _impl->resume();
        }

        inline Coroutine::Status Coroutine::status() const
        {
            _SPARK_ASSERT(_impl);
            return _impl->get_status();
        }

        inline void Coroutine::yield()
        {
            _Detail::CoroutineData* data = (_Detail::CoroutineData*)::GetFiberData();
            data->current_fiber ? data->current_fiber->yield() : data->major_fiber->yield();
        }

        template<class _Fn>
        inline void Coroutine::yield_until(_Fn condition)
        {
            _Detail::CoroutineData* data = (_Detail::CoroutineData*)::GetFiberData();
            data->current_fiber ? data->current_fiber->yield_until(condition) : data->major_fiber->yield_until(condition);
        }

        inline void Coroutine::await(Coroutine& fiber)
        {
            _Detail::CoroutineData* data = (_Detail::CoroutineData*)::GetFiberData();
            _SPARK_ASSERT(data->current_fiber != fiber._impl.get());

            if (fiber.status() == Coroutine::Status::Supended)
                fiber.resume();

            while (fiber.status() == Coroutine::Status::Supended)
            {
                if (data->current_fiber)
                    Coroutine::yield();
                else
                    ::Sleep(1);

                fiber.resume();
            }
        }
    } 
}