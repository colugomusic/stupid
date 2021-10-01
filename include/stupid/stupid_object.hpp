#pragma once

#include <cassert>
#include "stupid_object_manager.hpp"

/*
* Everything here assumes there is one writer thread
* 
* Basic concept is that when the writer thread wants to modify an object in a way
* that needs to be synchronized, it instead creates a copy of the object and
* performs the modifications on the copy. It then calls writer().commit() to make the
* new version of the object available to the reader thread.
* 
* The reader thread should call reader().pending() to check if there is a new version
* of the object waiting to be picked up, and reader().get_next() to retrieve the new
* version.
* 
* The interface returned from writer() overloads the -> and * operators which can be used
* to access the most recently committed version of the object. This is the version
* which is used as a base by writer().make_copy().
* 
* The interface returned from reader() also overloads the -> and * operators, and can
* be used to access the most recently retrieved version of the object (which is not
* necessarily the most recently committed).
* 
* usage:
* 
*	stupid::Object<Thing> thing;
* 
* in writer thread:
* 
*	//
*	// initialization:
*	//
* 
*	thing.writer().commit(thing.writer().make_new());
* 
*	//	
*	// updating:
*	//
* 
*	// Copies the most recently commit()'ed version of the object
*	auto new_thing = thing.writer().make_copy(); 
*	
*	new_thing->a = 1;
*	new_thing->b = 2;
*	new_thing->c = 3;
*	
*	thing.writer().commit(new_thing);
* 
* in reader thread (1):
*	
*	if (thing.reader().pending())
*	{
*		thing.reader().get_next();
*	}
*		
*	thing.reader()->do_things();
* 
* in reader thread (2):
* 
*	Thing* ptr;
*	
*	...
* 
*	if (thing.reader().pending())
*	{
*		// resulting pointer must be explicitly disposed, otherwise
*		// there will be a memory leak
*		ptr = thing.reader().get_next_unmanaged();
*	}
* 
*	ptr->do_things();
*	
*	thing.dispose(ptr);
*/

namespace stupid {

template <class T> class Object;

template <class T>
class ObjectSetup
{
	friend class Object<T>;

	T* object_ = nullptr;

	ObjectSetup(T* source)
		: object_(new T(*source))
	{
	}

	template <class ...Args>
	ObjectSetup(Args ... args)
		: object_(new T(args...))
	{
	}

public:

	~ObjectSetup()
	{
		if (object_) delete object_;
	}

	T* operator->() { return object_; }
	T& operator*() { return *object_; }

	T* get() { return object_; }
};

/*
 * These Reader and Writer classes are simply used to create
 * a clear distinction between reader and writer operations at the call site. There
 * is nothing stopping clients from calling reader() in a writer thread or writer() in
 * a reader thread
 */

template <class T>
class Reader
{
	friend class Object<T>;

	Object<T>* object_;
	T* current_ = nullptr;
	T* retrieved_ = nullptr;

	Reader(Object<T>* object)
		: object_(object)
	{
	}

public:

	~Reader()
	{
		if (current_) object_->dispose(current_);
	}

	bool pending() const
	{
		return object_->pending();
	}

	T* get_next_if_pending()
	{
		return pending() ? get_next() : nullptr;
	}

	T* get_next()
	{
		if (current_) object_->dispose(current_);

		current_ = object_->get_next();

		retrieved_ = current_;

		return current_;
	}

	T* get_next_unmanaged()
	{
		return (retrieved_ = object_->get_next());
	}

	T* update()
	{
		if (pending())
		{
			return get_next();
		}

		// Could be null if the writer hasn't pushed anything yet
		return current_;
	}

	T* get() { return retrieved_; }
	const T* get() const { return retrieved_; }
	T* operator->() { return get(); }
	const T* operator->() const { return get(); }
	T& operator*() { return *get(); }
	const T& operator*() const { return *get(); }
};

template <class T>
class Writer
{
	friend class Object<T>;

	Object<T>* object_;

	Writer(Object<T>* object)
		: object_(object)
	{
	}

public:

	template <class ...Args>
	ObjectSetup<T> make_new(Args... args)
	{
		return object_->make_new(args...);
	}

	ObjectSetup<T> make_copy()
	{
		return object_->make_copy();
	}

	ObjectSetup<T> make_copy(T* source)
	{
		return object_->make_copy(source);
	}

	void commit(ObjectSetup<T>& setup)
	{
		object_->commit(setup);
	}

	template <class ...Args>
	void commit_new(Args... args)
	{
        auto new_object = make_new(args...);
        
		object_->commit(new_object);
	}

	T* get() { return object_->recent_; }
	const T* get() const { return object_->recent_; }
	T* operator->() { return get(); }
	const T* operator->() const { return get(); }
	T& operator*() { return *get(); }
	const T& operator*() const { return *get(); }

	operator bool() const { return object_->recent_; }
};

template <class T>
class Object
{
	friend class Reader<T>;
	friend class Writer<T>;

	ObjectManager<T> manager_;

	Reader<T> reader_interface_;
	Writer<T> writer_interface_;
	std::atomic<T*> next_ = nullptr;
	T* recent_ = nullptr;

	template <class ...Args>
	ObjectSetup<T> make_new(Args... args)
	{
		return ObjectSetup<T>(args...);
	}

	ObjectSetup<T> make_copy()
	{
		assert(recent_);

		return ObjectSetup<T>(*recent_);
	}

	ObjectSetup<T> make_copy(T* source)
	{
		return ObjectSetup<T>(*source);
	}

	void commit(ObjectSetup<T>& setup)
	{
		recent_ = setup.object_;

		setup.object_ = nullptr;

		manager_.add(recent_);

		auto prev_next = next_.exchange(recent_);

		if (prev_next) manager_.dispose(prev_next);

		manager_.collect();
	}

	bool pending() const { return next_; }
	T* get_next() { return next_.exchange(nullptr); }

public:

	Object()
		: reader_interface_(this)
		, writer_interface_(this)
	{
	}

	~Object()
	{
		if (next_) manager_.dispose(next_);

		manager_.collect();
	}

	Reader<T>& reader() { return reader_interface_; }
	const Reader<T>& reader() const { return reader_interface_; }
	Writer<T>& writer() { return writer_interface_; }
	const Writer<T>& writer() const { return writer_interface_; }

	void dispose(T* object)
	{
		manager_.dispose(object);
	}
};

} // stupid