#include <atomic>
#include <map>

namespace stupid {
namespace experimental {

template <class T> class Book;
template <class T> class Object;

template <class T>
class Record
{
public:

    Record(T* data, Book<T>* book)
        : data_(data)
        , book_(book)
    {
    }

    void ref()
    {
        ref_count_++;
    }

    void unref()
    {
        if (--ref_count_ == 0) book_->dispose(this);
    }

private:

    T* data_ { nullptr };
    Book<T>* book_ { nullptr };
    std::atomic<int> ref_count_ { 0 };
};

template <class T>
class Immutable
{
public:

    Immutable() = default;

    Immutable(Record* record)
        : record_(record)
    {
        if (record_) record_->ref();
    }

    ~Immutable()
    {
        if (record_) record_->unref();
    }

    Immutable(const Immutable& rhs)
        : record_(rhs.record_)
    {
        if (record_) record_->ref();
    }

    Immutable& operator=(const Immutable& rhs)
    {
        record_ = rhs.record_;

        if (record_) record_->ref();
    }

    operator bool() const { return record_; }

    const T* get() const
    {
        assert(record_);

        return record_->object;
    }

    const T* operator->() const { return get(); }
    const T& operator*() const { return *(get()); }

private:

    Record* record_ { nullptr };
};

template <class T>
class Book
{
public:

    ~Book()
    {
        collect();
    }

    Record<T>* make_record(T* data)
    {
        const auto out = new Record<T> { data, this };

        dispose_flags_[out] = false;

        return out;
    }

    void dispose(Record<T>* record)
    {
        const auto pos = dispose_flags_.find(record);

		if (pos != dispose_flags_.end())
		{
			pos->second = true;
		}
    }

	void collect()
	{
		for (auto pos = dispose_flags_.begin(); pos != dispose_flags_.end();)
		{
            const auto record = pos->first;
            const auto disposed = pos->second.load();

			if (disposed && record->ref_count.load() == 0)
			{
                delete record->object;
				delete record;

				pos = dispose_flags_.erase(pos);
			}
			else
			{
				pos++;
			}
		}
	}
    
private:

	std::map<Record<T>*, std::atomic_bool> dispose_flags_;
};

template <class T>
class Read
{
	friend class Object<T>;

public:

    Immutable<T> get()
    {
		retrieved_ = object_->get();

		return retrieved_;
    }

    const Immutable<T> get() const
    {
		return retrieved_;
    }

private:

	Read(Object<T>* object)
		: object_(object)
	{
	}

	Object<T>* object_;
	Immutable<T> retrieved_;

};

template <class T>
class Write
{
	friend class Object<T>;

public:

    T* copy() { return object_->copy(); }
    void commit(T* data) { object_->commit(data); }

	Immutable<T> get() { return Immutable<T> { object_->last_written_record_.load() }; }
	const Immutable<T> get() const { return Immutable<T> { object_->last_written_record_.load() }; }
    
private:

	Write(Object<T>* object)
		: object_(object)
	{
	}

	Object<T>* object_;
};

template <class T>
class Object
{
    friend class Read<T>;
    friend class Write<T>;

public:

    Object()
        : read_(this)
        , write_(this)
    {
    }

    Read<T>& read() { return read_; }
    const Read<T>& read() const { return read_; }

    Write<T>& write() { return write_; }
    const Write<T>& write() const { return write_; }

private:

    Immutable<T> get()
    {
        return Immutable<T> { last_written_record_.load() };
    }

    T* copy()
    {
        Immutable<T> ref { last_written_record_.load() };

        if (!ref) return nullptr;
        
        return new T(*(ref.get()));
    }

    void commit(T* data)
    {
        const auto record = book_.make_record(data);

        last_written_record_.store(record);
        last_written_ = Immutable<T> { record };

        book_.collect();
    }

    Read<T> read_;
    Write<T> write_;

    Book book_;

    std::atomic<Record<T>*> last_written_record_ { nullptr };

    // Keep at least one reference until overwritten
    Immutable<T> last_written_ref_;
};

} // experimental
} // stupid