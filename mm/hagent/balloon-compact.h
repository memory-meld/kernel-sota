#ifndef BALLOON_COMPACT_H
#define BALLOON_COMPACT_H
#include <linux/virtio_config.h>

#undef __cleanup
#define __cleanup(func) __maybe_unused __attribute__((__cleanup__(func)))

#define __guard_ptr(_name) class_##_name##_lock_ptr

#define scoped_guard(_name, args...)					\
	for (CLASS(_name, scope)(args),					\
	     *done = NULL; __guard_ptr(_name)(&scope) && !done; done = (void *)1)

#define CLASS(_name, var)						\
	class_##_name##_t var __cleanup(class_##_name##_destructor) =	\
		class_##_name##_constructor


#define __DEFINE_UNLOCK_GUARD(_name, _type, _unlock, ...)		\
typedef struct {							\
	_type *lock;							\
	__VA_ARGS__;							\
} class_##_name##_t;							\
									\
static inline void class_##_name##_destructor(class_##_name##_t *_T)	\
{									\
	if (_T->lock) { _unlock; }					\
}									\
									\
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T)	\
{									\
	return _T->lock;						\
}

#define __DEFINE_LOCK_GUARD_1(_name, _type, _lock)			\
static inline class_##_name##_t class_##_name##_constructor(_type *l)	\
{									\
	class_##_name##_t _t = { .lock = l }, *_T = &_t;		\
	_lock;								\
	return _t;							\
}

#define DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, ...)		\
__DEFINE_UNLOCK_GUARD(_name, _type, _unlock, __VA_ARGS__)		\
__DEFINE_LOCK_GUARD_1(_name, _type, _lock)

DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t,
		    spin_lock_irqsave(_T->lock, _T->flags),
		    spin_unlock_irqrestore(_T->lock, _T->flags),
		    unsigned long flags)

void virtio_reset_device(struct virtio_device *dev)
{
	dev->config->reset(dev);
}

#define last_node(src) __last_node(&(src))
static inline unsigned int __last_node(const nodemask_t *srcp)
{
	return min_t(unsigned int, MAX_NUMNODES, find_last_bit(srcp->bits, MAX_NUMNODES));
}

#endif
