#pragma once

#include <atomic>
#include <cassert>
#include <vector>

namespace silly {

template <typename T> class ref;

namespace detail {

template <typename T>
struct control_block
{
	const T value;
	std::atomic<uint32_t> ref_count{0};
};

} // detail

template <typename T>
class object
{
public:

	using cb_t = detail::control_block<T>;
	using me_t = object<T>;
	using ref_t = ref<T>;

	object(const object&) = delete;
	object(object&&) = delete;
	auto operator=(const object&) -> object& = delete;
	auto operator=(object&&) -> object& = delete;

	template <typename... Args>
	object(Args... args) : critical_{args...} {}

private:

	struct critical_t
	{
		template <typename... Args>
		critical_t(Args... args) : control_block{new cb_t{T{args...}, 0}} {}

		std::atomic<cb_t*> control_block;
	} critical_;

public:

	struct read_t
	{
		read_t(me_t* self) : self_{self} {}

		auto acquire() const -> ref_t
		{
			const auto cb{self_->critical_.control_block.load()};

			assert (cb);

			return ref_t{cb};
		}

		auto get_value() const -> T
		{
			const auto cb{self_->critical_.control_block.load()};

			assert (cb);

			return cb->value;
		}

	private:

		me_t* self_;
	} read{this};

	struct write_t
	{
		write_t(me_t* self)
			: self_{self}
		{
			instance_ = ref_t{self_->critical_.control_block.load()};
		}

		template <typename U>
		auto set(U&& value) -> void
		{
			// The old control block won't be reclaimed before
			// the end of this function, because we keep this
			// reference to it. So it won't be garbage collected
			// yet when we call garbage_collect() at the end of
			// this function.
			const auto old_instance{instance_};

			// Create the new control block
			const auto cb{new cb_t{std::forward<U>(value), 0}};

			// Atomically set the new control block
			self_->critical_.control_block = cb;

			// Keep a reference to the new control block
			instance_ = ref_t{cb};

			// Push the old control block onto the garbage. It
			// won't be collected yet.
			garbage_.push_back(old_instance);

			// Collect old control blocks that were already
			// discarded.
			garbage_collect();
		}

		template <typename UpdateFn>
		auto update(UpdateFn&& fn) -> void
		{
			set(fn(*instance_));
		}

	private:

		auto garbage_collect() -> void
		{
			static const auto can_be_deleted = [](const ref_t& instance)
			{
				assert (instance.cb_);
				assert (instance.cb_ > 0);

				return instance.cb_->ref_count == 1;
			};

			garbage_.erase(std::remove_if(std::begin(garbage_), std::end(garbage_), can_be_deleted), std::end(garbage_));
		}

		me_t* self_;
		ref_t instance_;
		std::vector<ref_t> garbage_;
	} write{this};
};

template <typename T>
class ref
{
public:

	using cb_t = detail::control_block<T>;

	ref() = default;

	ref(cb_t* cb)
		: cb_{cb}
	{
		assert (cb);

		ref_add();
	}

	ref(ref<T>&& rhs) noexcept
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;
		rhs.cb_ = {};
	}

	ref(const ref<T>& rhs)
		: cb_{rhs.cb_}
	{
		if (!cb_) return;

		assert (cb_->ref_count > 0);

		ref_add();
	}

	~ref()
	{
		if (!cb_) return;

		ref_sub();
	}

	auto operator=(ref<T>&& rhs) noexcept -> ref<T>&
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;
		rhs.cb_ = {};

		return *this;
	}

	auto operator=(const ref<T>& rhs) -> ref<T>&
	{
		if (cb_)
		{
			ref_sub();
		}

		cb_ = rhs.cb_;

		if (!cb_) return *this;

		assert (cb_->ref_count > 0);

		ref_add();

		return *this;
	}

	auto& get_value() const { return cb_->value; }
	auto operator*() const -> const T& { return cb_->value; }
	auto operator->() const -> const T* { return &cb_->value; }

private:

	auto ref_add() -> void
	{
		assert (cb_);

		cb_->ref_count++;
	}

	auto ref_sub() -> void
	{
		assert (cb_);

		if (cb_->ref_count.fetch_sub(1) == 1)
		{
			delete cb_;
		}
	}

	cb_t* cb_{};

	friend class object<T>;
};

/////////////////////////////////////////////////////////////////////////
/// sync signal /////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

class sync_signal
{
public:

	auto get_value() const { return value_; }
	auto notify() -> void { value_++; }

private:

	uint32_t value_{0};
};

/////////////////////////////////////////////////////////////////////////
/// signal synced object ////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////

template <class T, class SignalType = sync_signal>
class signal_synced_object
{
public:

	using me_t = signal_synced_object<T, SignalType>;
	using object_t = object<T>;
	using ref_t = ref<T>;

	template <typename... Args>
	signal_synced_object(const SignalType& signal, Args... args)
		: read{this, signal}
		, critical_{args...}
	{
	}

	struct read_t
	{
		read_t(me_t* self, const SignalType& signal) : self_{self}, signal_{&signal} {}

		auto acquire() -> ref_t
		{
			update();

			return current_;
		}

		auto is_data_pending() const -> bool
		{
			return self_->critical_->data_pending;
		}

	private:

		auto update() -> void
		{
			const auto signal_value{signal_->get_value()};

			if (signal_value > slot_value_)
			{
				const auto data_pending{self_->critical_.data_pending.exchange(false)};

				if (data_pending)
				{
					current_ = self_->critical_.object.read.acquire();
				}
			}

			slot_value_ = signal_value;
		}

		const SignalType* signal_;
		uint32_t slot_value_{0};
		ref_t current_;
		me_t* self_;
	} read;

	struct write_t
	{
		write_t(me_t* self) : self_{self} {}

		template <typename UpdateFn>
		auto update(UpdateFn&& fn) -> void
		{
			self_->critical_.object.write.update(std::forward<UpdateFn>(fn));
			self_->critical_.data_pending = true;
		}

	private:

		me_t* self_;
	} write{this};

private:

	struct critical_t
	{
		template <typename... Args>
		critical_t(Args... args) : object{args...} {}

		object_t object;
		std::atomic_bool data_pending;
	} critical_;
};

} // silly