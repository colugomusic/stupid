# stupid
stupid lock-free synchronization

The base functionality of this library is provided by the classes:
* `stupid::Object<T>`
* `stupid::Immutable<T>`

Some additional classes are provided for more specific use cases:
* `stupid::SyncSignal`
* `stupid::SignalSyncObject<T>`
* `stupid::SignalSyncObjectPair<T>`
* `stupid::QuickSync<T>`

## Very basic usage

```c++
stupid::Object<Thing> thing;

/**** In the writer thread ****/

// Initialize the object
thing.write().commit_new();

// That's equivalent to doing this
thing.write().commit(new Thing{});

// Create a copy of the object to modify
auto copy = thing.write().copy();

copy->modify();
copy->the();
copy->object();

// Commit the modified object
thing.write().commit(copy);

/**** In the reader thread ****/

// Get a reference to the most recently committed version of the object
stupid::Immutable<Thing> ref = thing.read().get();

ref->access();
ref->read_only();
ref->stuff();

// stupid::Immutable is a reference counted type. The referenced version of
// the object will remain valid as long as there are references to it, even
// if the writer thread commits new versions of the object.

```

## Caveats
* Only one simultaneous writer thread is supported
* All `stupid::Immutable`'s for a `stupid::Object` must be deleted before the `stupid::Object` is destructed

## Notes
* Only these methods allocate memory:
    - `stupid::Write::commit_new()`
    - `stupid::Write::copy()`

* Only these methods deallocate memory (in the form of garbage collection of old versions of the object):
    - `stupid::Object::~Object()`
    - `stupid::Write::commit()`
    - `stupid::Write::commit_new()`
 
* Memory is deallocated using `delete`
