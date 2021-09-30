#pragma once

#include "stupid_object.hpp"
#include "stupid_signal.hpp"

namespace stupid {

template <class T>
class SyncValue
{

public:

	SyncValue(const SyncSignal& signal)
		: signal_(&signal)
	{
	}

	T reader()
	{
		if (signal_->value() > slot_value_)
		{
			buffer_value_ = value_.load();
			slot_value_ = signal_->value();
		}

		return buffer_value_;
	}

	T writer()
	{
		return value_.load();
	}

	void set(T value)
	{
		value_.store(value);
	}

private:

	const Signal* signal_;
	std::uint32_t slot_value_ = 0;
	std::atomic<T> value_;
	T buffer_value_;
};

template <class T>
class SignalSyncObject
{

public:

	SignalSyncObject(const SyncSignal& signal)
		: signal_(&signal)
	{
	}

	T* reader()
	{
		if (signal_->value() > slot_value_)
		{
			object_.reader().get_next_if_pending();
			slot_value_ = signal_->value();
		}

		return object_.reader().get();
	}

	Writer<T>& writer()
	{
		return object_.writer();
	}

	const Writer<T>& writer() const
	{
		return object_.writer();
	}

	bool pending() const
	{
		return object_.reader().pending();
	}

	ObjectSetup<T> make_copy()
	{
		return object_.writer().make_copy();
	}

	void commit(ObjectSetup<T>& setup)
	{
		object_.writer().commit(setup);
	}

	template <class ...Args>
	void commit_new(Args... args)
	{
		object_.writer().commit_new(args...);
	}

	T& operator*() { return object_; }
	const T& operator*() const { return object_; }
	T* operator->() { return &object_; }
	const T* operator->() const { return &object_; }

private:

	const SyncSignal* signal_ = nullptr;
	std::uint32_t slot_value_ = 0;
	Object<T> object_;
};

template <class T>
class SignalSyncObjectPair
{
public:

	SignalSyncObjectPair(const SyncSignal& signal)
		: signal_(&signal)
	{
	}

	~SignalSyncObjectPair()
	{
		if (current_[0]) object_.dispose(current_[0]);
		if (current_[1]) object_.dispose(current_[1]);
	}

	void update(int idx)
	{
		if (signal_->value() > slot_value_)
		{
			if (object_.reader().pending())
			{
				if (current_[idx] && current_[idx] != current_[flip(idx)])
				{
					object_.dispose(current_[idx]);
				}

				current_[idx] = object_.reader().get_next_unmanaged();
				recent_ = current_[idx];
			}
		}
	}

	T* reader(int idx) const
	{
		if (current_[idx]) return current_[idx];
		if (current_[flip(idx)]) return current_[flip(idx)];

		return nullptr;
	}

	T* recent() { return recent_; }
	const T* recent() const { return recent_; }

	Writer<T>& writer()
	{
		return object_.writer();
	}

	const Writer<T>& writer() const
	{
		return object_.writer();
	}

	bool pending() const
	{
		return object_.reader().pending();
	}

	ObjectSetup<T> make_new()
	{
		return object_.writer().make_new();
	}

	ObjectSetup<T> make_copy()
	{
		return object_.writer().make_copy();
	}

	void commit(ObjectSetup<T>& setup)
	{
		object_.writer().commit(setup);
	}

	template <class ...Args>
	void commit_new(Args... args)
	{
		object_.writer().commit_new(args...);
	}

private:

	static int flip(int x) { return 1 - x; }

	const SyncSignal* signal_ = nullptr;
	std::uint32_t slot_value_ = 0;
	Object<T> object_;
	T* current_[2] = { nullptr, nullptr };
	T* recent_ = nullptr;
};

}}
