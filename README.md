# libnrbf

Clean-room reader for .NET Binary Formatter ("NRBF") streams in C++17.

Implements a useful subset of [MS-NRBP](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-nrbp/), Microsoft's published Open Specification for the format. Streaming visitor API: no full in-memory object graph is built, so it works on multi-gigabyte streams.

## Status

1.0. Stable API.

## Supported records

- `SerializedStreamHeader`, `MessageEnd`
- `BinaryLibrary`
- `ClassWithMembersAndTypes`, `SystemClassWithMembersAndTypes`, `ClassWithId`
- `BinaryObjectString`, `MemberReference`, `MemberPrimitiveTyped`
- `ObjectNull`, `ObjectNullMultiple256`, `ObjectNullMultiple`
- `ArraySinglePrimitive`, `ArraySingleObject`, `ArraySingleString`
- `BinaryArray`: primitive and object-bearing variants both dispatch through the visitor (the primitive payload reads the same way as `ArraySinglePrimitive`; object arrays fire `enter_object_array` / per-record dispatch / `exit_object_array`).

Not supported (rare in serialized payloads): deprecated `ClassWithMembers` / `SystemClassWithMembers`, `MethodCall`, `MethodReturn`.

## Visitor surface

Override only the callbacks you need; defaults are no-ops. Return `false` from `enter_*` to skip a subtree.

| Callback | Fires for |
|---|---|
| `enter_instance(id, ClassDef)` / `exit_instance` | Each class instance (returns bool to descend) |
| `member(ClassMember, Value)` | Each typed member of a descended instance |
| `string_object(id, view)` | `BinaryObjectString` records (resolves forward `MemberReference`s) |
| `enter_object_array(id, length)` / `exit_object_array` | `ArraySingleObject` and `BinaryArray` (object variant) |
| `enter_primitive_array(id, PrimitiveType, length)` / `primitive_array_value(idx, Value)` / `exit_primitive_array` | `ArraySinglePrimitive` and `BinaryArray` (primitive variant), used for `byte[]` payloads, etc. |
| `enter_string_array(id, length)` / `string_array_value(idx, view)` / `exit_string_array` | `ArraySingleString` |

`Value` is a small variant carrying `kind` (`Bool` / `Int` / `UInt` / `Double` / `String` / `ObjectRef` / `DateTime` / `Decimal`) plus the relevant payload (`i` / `u` / `d` / `s` / `object_id`). `DateTime` arrives as the raw 64-bit .NET bits: top 2 bits encode `Kind`, low 62 are 100-ns ticks since year 1.

## Forward references

`BinaryObjectString` records can appear *after* the `MemberReference` that points at them. This is typical when an object graph uses a shared string table at the tail of the stream. Resolve by tracking the `object_id` from the reference and waiting for the matching `string_object(id, …)` callback. The bundled `tests/test_dump` shows the pattern.

## Diagnostics tool

`tests/test_dump` walks the entire record stream and prints structure + distinct values per member. Useful for schema discovery on unknown .NET-serialized payloads.

```sh
build/test_dump path/to/payload.bin            # summary
build/test_dump path/to/payload.bin --full     # all distinct values
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Use from CMake

```cmake
add_subdirectory(extern/libnrbf)   # or FetchContent / submodule
target_link_libraries(myapp PRIVATE nrbf::nrbf)
```

```cpp
#include <nrbf/nrbf.hpp>

class MyVisitor : public nrbf::Visitor {
    bool enter_instance(int32_t id, const nrbf::ClassDef& d) override {
        if (d.name == "MyClass") { /* collect fields */ return true; }
        return false;   // skip subtree
    }
    void member(const nrbf::ClassMember& m, const nrbf::Value& v) override {
        // record m.name + v.kind/v.i/v.s/...
    }
};

void parse_file(std::string_view bytes) {
    MyVisitor v;
    nrbf::parse(bytes, v);   // throws nrbf::ParseError on bad input
}
```

`nrbf::dump(bytes, out)` writes a human-readable tree to a string. Handy for schema discovery.

## License

MIT.
