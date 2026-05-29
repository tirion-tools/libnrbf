// SPDX-License-Identifier: MIT
// libnrbf - clean-room reader for .NET Binary Formatter (NRBF) streams.
// Implements a useful subset of MS-NRBP (the published Microsoft Open
// Specification for the format). Streaming visitor API - no full in-memory
// object graph is built, so it works on multi-gigabyte streams.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nrbf {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// MS-NRBP 2.1.2.3 PrimitiveTypeEnumeration
enum class PrimitiveType : uint8_t {
    None = 0, Boolean = 1, Byte = 2, Char = 3,
    Decimal = 5, Double = 6, Int16 = 7, Int32 = 8, Int64 = 9,
    SByte = 10, Single = 11, TimeSpan = 12, DateTime = 13,
    UInt16 = 14, UInt32 = 15, UInt64 = 16, Null = 17, String = 18,
};

// MS-NRBP 2.1.2.2 BinaryTypeEnumeration
enum class BinaryType : uint8_t {
    Primitive = 0, String = 1, Object = 2,
    SystemClass = 3, Class = 4,
    ObjectArray = 5, StringArray = 6, PrimitiveArray = 7,
};

struct ClassMember {
    std::string name;
    BinaryType binary_type = BinaryType::Object;
    PrimitiveType prim_type = PrimitiveType::None;
    std::string class_name;   // for SystemClass / Class
    int32_t library_id = 0;   // for Class
};

struct ClassDef {
    int32_t object_id = 0;
    int32_t library_id = 0;
    std::string library_name;     // resolved from library_id
    std::string name;
    bool system_class = false;    // true for SystemClassWithMembersAndTypes
    std::vector<ClassMember> members;
};

// A single field value passed to the visitor. The active union field is
// determined by `kind`. References to other objects are reported by id.
struct Value {
    enum class Kind {
        Null,
        Bool,
        Int,        // signed integers (Int16/32/64, SByte)
        UInt,       // unsigned integers (Byte, UInt16/32/64, Char-as-codepoint)
        Float,      // 32-bit
        Double,     // 64-bit
        String,
        Decimal,    // exact-decimal string as captured
        DateTime,   // 64-bit raw bits (ticks + kind)
        TimeSpan,   // 64-bit ticks
        ObjectRef,  // points to an object_id elsewhere in the stream
    };
    Kind kind = Kind::Null;
    int64_t i = 0;
    uint64_t u = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    std::string s;
    int32_t object_id = 0;
};

// Visitor - override what you care about; default impls are no-ops or
// "descend into children". Return false from enter_* to skip a subtree
// (the parser still consumes the bytes - your data model just won't
// receive the inner reports).
class Visitor {
public:
    virtual ~Visitor() = default;

    virtual void library(int32_t library_id, std::string_view name) {
        (void)library_id; (void)name;
    }

    // ── Class instance ────────────────────────────────────────────────────
    virtual bool enter_instance(int32_t object_id, const ClassDef& def) {
        (void)object_id; (void)def; return true;
    }
    virtual void member(const ClassMember& m, const Value& v) {
        (void)m; (void)v;
    }
    virtual void exit_instance(int32_t object_id, const ClassDef& def) {
        (void)object_id; (void)def;
    }

    // ── Standalone string object (BinaryObjectString) ─────────────────────
    virtual void string_object(int32_t object_id, std::string_view s) {
        (void)object_id; (void)s;
    }

    // ── Standalone primitive (MemberPrimitiveTyped) ───────────────────────
    virtual void primitive(PrimitiveType pt, const Value& v) {
        (void)pt; (void)v;
    }

    // ── Arrays ────────────────────────────────────────────────────────────
    virtual bool enter_primitive_array(int32_t object_id,
                                       PrimitiveType pt,
                                       int32_t length) {
        (void)object_id; (void)pt; (void)length; return false;
    }
    virtual void primitive_array_value(int32_t index, const Value& v) {
        (void)index; (void)v;
    }
    virtual void exit_primitive_array(int32_t object_id) {
        (void)object_id;
    }

    virtual bool enter_object_array(int32_t object_id, int32_t length) {
        (void)object_id; (void)length; return true;
    }
    virtual void exit_object_array(int32_t object_id) {
        (void)object_id;
    }

    virtual bool enter_string_array(int32_t object_id, int32_t length) {
        (void)object_id; (void)length; return false;
    }
    virtual void string_array_value(int32_t index, std::string_view s) {
        (void)index; (void)s;
    }
    virtual void exit_string_array(int32_t object_id) {
        (void)object_id;
    }
};

// Fan-out composition: one parse feeds every registered child, so callers
// avoid re-parsing a stream per consumer. enter_* returns the OR of the
// children's returns, so the parser descends if any child wants to.
// Adding a Visitor virtual needs a matching override here; test_multi guards it.
class MultiVisitor final : public Visitor {
public:
    void add(Visitor& v) { children_.push_back(&v); }

    void library(int32_t library_id, std::string_view name) override {
        for (auto* c : children_) c->library(library_id, name);
    }

    bool enter_instance(int32_t object_id, const ClassDef& def) override {
        bool any = false;
        for (auto* c : children_)
            if (c->enter_instance(object_id, def)) any = true;
        return any;
    }
    void member(const ClassMember& m, const Value& v) override {
        for (auto* c : children_) c->member(m, v);
    }
    void exit_instance(int32_t object_id, const ClassDef& def) override {
        for (auto* c : children_) c->exit_instance(object_id, def);
    }

    void string_object(int32_t object_id, std::string_view s) override {
        for (auto* c : children_) c->string_object(object_id, s);
    }
    void primitive(PrimitiveType pt, const Value& v) override {
        for (auto* c : children_) c->primitive(pt, v);
    }

    bool enter_primitive_array(int32_t object_id,
                               PrimitiveType pt,
                               int32_t length) override {
        bool any = false;
        for (auto* c : children_)
            if (c->enter_primitive_array(object_id, pt, length)) any = true;
        return any;
    }
    void primitive_array_value(int32_t index, const Value& v) override {
        for (auto* c : children_) c->primitive_array_value(index, v);
    }
    void exit_primitive_array(int32_t object_id) override {
        for (auto* c : children_) c->exit_primitive_array(object_id);
    }

    bool enter_object_array(int32_t object_id, int32_t length) override {
        bool any = false;
        for (auto* c : children_)
            if (c->enter_object_array(object_id, length)) any = true;
        return any;
    }
    void exit_object_array(int32_t object_id) override {
        for (auto* c : children_) c->exit_object_array(object_id);
    }

    bool enter_string_array(int32_t object_id, int32_t length) override {
        bool any = false;
        for (auto* c : children_)
            if (c->enter_string_array(object_id, length)) any = true;
        return any;
    }
    void string_array_value(int32_t index, std::string_view s) override {
        for (auto* c : children_) c->string_array_value(index, s);
    }
    void exit_string_array(int32_t object_id) override {
        for (auto* c : children_) c->exit_string_array(object_id);
    }

private:
    std::vector<Visitor*> children_;
};

// Parse an NRBF stream, invoking `v` for every record. Throws ParseError
// on malformed input.
void parse(std::string_view bytes, Visitor& v);

// Dump the stream as human-readable text to `out`. Long strings are
// truncated to `max_str_len`. Useful for schema discovery.
void dump(std::string_view bytes, std::string& out, size_t max_str_len = 80);

}  // namespace nrbf
