#pragma once

#include "stupid_garbage_disposal.hpp"

namespace stupid {

template <class T>
class ObjectManager
{
	GarbageDisposal<T> disposal_;

public:

	template <class ...Args>
	T* make_new(Args... args)
	{
		return add(new T(args...));
	}

	T* make_copy(T* source)
	{
		return add(new T(*source));
	}

	T* add(T* object)
	{
		return disposal_.create_entry(object);
	}

	void collect()
	{
		disposal_.collect();
	}

	void dispose(T* object)
	{
		disposal_.dispose(object);
	}
};

} // stupid
