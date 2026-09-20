/*
 * qjs_win_compat.c - Windows 平台互斥锁实现（qjs_native.h 中声明的 impl 函数）
 */
#ifndef _WIN32
#include "qjs_native.h"
int qjs_win_compat_not_used;   /* 防止空翻译单元告警 */
#else

#include "qjs_native.h"

void
qjs_mutex_init_impl(qjs_mutex_t *m)
{
    m->use_pthread = 0;
    InitializeCriticalSection(&m->u.cs);
}

void
qjs_mutex_lock_impl(qjs_mutex_t *m)
{
    EnterCriticalSection(&m->u.cs);
}

void
qjs_mutex_unlock_impl(qjs_mutex_t *m)
{
    LeaveCriticalSection(&m->u.cs);
}

void
qjs_mutex_destroy_impl(qjs_mutex_t *m)
{
    DeleteCriticalSection(&m->u.cs);
}

#endif /* _WIN32 */