// SPDX-License-Identifier: MIT
//
// MS-NRBP reference: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-nrbp/
// Sections cited in comments below refer to that spec.

#include "nrbf/nrbf.hpp"
#include "nrbf/profile.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace nrbf {

namespace {

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : data(p), size(n) {}

    size_t pos() const { return p_; }
    bool eof() const { return p_ >= size; }
    size_t remaining() const { return p_ <= size ? size - p_ : 0; }

    void need(size_t n) {
        if (p_ + n > size) {
            throw ParseError("Unexpected end of NRBF stream at offset " +
                             std::to_string(p_));
        }
    }

    uint8_t  u8()  { need(1); return data[p_++]; }
    uint16_t u16() {
        need(2);
        uint16_t v = uint16_t(data[p_]) |
                     (uint16_t(data[p_ + 1]) << 8);
        p_ += 2; return v;
    }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(data[p_])       |
                     (uint32_t(data[p_ + 1]) << 8)  |
                     (uint32_t(data[p_ + 2]) << 16) |
                     (uint32_t(data[p_ + 3]) << 24);
        p_ += 4; return v;
    }
    uint64_t u64() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(data[p_ + i]) << (8 * i);
        p_ += 8; return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    int64_t i64() { return static_cast<int64_t>(u64()); }
    float f32() {
        uint32_t b = u32();
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }
    double f64() {
        uint64_t b = u64();
        double f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }

    // MS-NRBP 2.1.1.6 LengthPrefixedString - up to 5-byte 7-bit-varint length.
    uint32_t lpstr_len() {
        uint32_t len = 0;
        int shift = 0;
        for (int i = 0; i < 5; ++i) {
            uint8_t b = u8();
            len |= uint32_t(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return len;
            shift += 7;
        }
        throw ParseError("Malformed length prefix");
    }

    std::string lpstr() {
        uint32_t len = lpstr_len();
        std::string s;
        if (len > 0) {
            need(len);
            s.assign(reinterpret_cast<const char*>(data + p_), len);
            p_ += len;
        }
        return s;
    }

    // View of the next LengthPrefixedString into the input bytes; no copy.
    std::string_view lpstr_view() {
        uint32_t len = lpstr_len();
        if (len == 0) return {};
        return view(len);
    }

    std::string_view view(size_t n) {
        need(n);
        std::string_view sv(reinterpret_cast<const char*>(data + p_), n);
        p_ += n;
        return sv;
    }

private:
    const uint8_t* data;
    size_t size;
    size_t p_ = 0;
};

// Bytes consumed by a primitive of the given type. For variable-length
// types (Char/Decimal/String) returns 0 - caller must handle them inline.
int primitive_fixed_size(PrimitiveType t) {
    switch (t) {
    case PrimitiveType::Boolean:
    case PrimitiveType::Byte:
    case PrimitiveType::SByte:   return 1;
    case PrimitiveType::Int16:
    case PrimitiveType::UInt16:  return 2;
    case PrimitiveType::Int32:
    case PrimitiveType::UInt32:
    case PrimitiveType::Single:  return 4;
    case PrimitiveType::Int64:
    case PrimitiveType::UInt64:
    case PrimitiveType::Double:
    case PrimitiveType::DateTime:
    case PrimitiveType::TimeSpan: return 8;
    default: return 0;
    }
}

Value read_primitive(Reader& r, PrimitiveType t) {
    Value v;
    switch (t) {
    case PrimitiveType::Boolean:
        v.kind = Value::Kind::Bool;
        v.i = (r.u8() != 0) ? 1 : 0;
        break;
    case PrimitiveType::Byte:
        v.kind = Value::Kind::UInt;
        v.u = r.u8();
        break;
    case PrimitiveType::SByte:
        v.kind = Value::Kind::Int;
        v.i = static_cast<int8_t>(r.u8());
        break;
    case PrimitiveType::Int16:
        v.kind = Value::Kind::Int;
        v.i = static_cast<int16_t>(r.u16());
        break;
    case PrimitiveType::UInt16:
        v.kind = Value::Kind::UInt;
        v.u = r.u16();
        break;
    case PrimitiveType::Int32:
        v.kind = Value::Kind::Int;
        v.i = r.i32();
        break;
    case PrimitiveType::UInt32:
        v.kind = Value::Kind::UInt;
        v.u = r.u32();
        break;
    case PrimitiveType::Int64:
        v.kind = Value::Kind::Int;
        v.i = r.i64();
        break;
    case PrimitiveType::UInt64:
        v.kind = Value::Kind::UInt;
        v.u = r.u64();
        break;
    case PrimitiveType::Single:
        v.kind = Value::Kind::Float;
        v.f32 = r.f32();
        break;
    case PrimitiveType::Double:
        v.kind = Value::Kind::Double;
        v.f64 = r.f64();
        break;
    case PrimitiveType::DateTime:
        v.kind = Value::Kind::DateTime;
        v.u = r.u64();
        break;
    case PrimitiveType::TimeSpan:
        v.kind = Value::Kind::TimeSpan;
        v.i = r.i64();
        break;
    case PrimitiveType::Char: {
        // UTF-8 encoded codepoint, 1-4 bytes determined by lead byte.
        uint8_t lead = r.u8();
        size_t extra = 0;
        if ((lead & 0x80) == 0)        extra = 0;
        else if ((lead & 0xE0) == 0xC0) extra = 1;
        else if ((lead & 0xF0) == 0xE0) extra = 2;
        else if ((lead & 0xF8) == 0xF0) extra = 3;
        else throw ParseError("Bad UTF-8 lead byte in Char primitive");
        std::string s(1, char(lead));
        if (extra) s += std::string(r.view(extra));
        v.kind = Value::Kind::String;
        v.s = std::move(s);
        break;
    }
    case PrimitiveType::Decimal:
        v.kind = Value::Kind::Decimal;
        v.s = r.lpstr();
        break;
    case PrimitiveType::String:
        v.kind = Value::Kind::String;
        v.s = r.lpstr();
        break;
    case PrimitiveType::Null:
        v.kind = Value::Kind::Null;
        break;
    default:
        throw ParseError("Unknown PrimitiveType " +
                         std::to_string(int(t)));
    }
    return v;
}

class Parser {
public:
    Parser(const uint8_t* data, size_t size, Visitor& v)
        : r_(data, size), v_(v) {}

    void run() {
        // A single blob may contain multiple back-to-back NRBF
        // messages (each its own header → MessageEnd record), so we
        // loop until the stream is exhausted.
        while (!r_.eof()) {
            uint8_t hdr = r_.u8();
            if (hdr != 0) {
                throw ParseError("Expected SerializedStreamHeader at offset " +
                                 std::to_string(r_.pos() - 1));
            }
            (void)r_.i32(); // RootId
            (void)r_.i32(); // HeaderId
            (void)r_.i32(); // MajorVersion
            (void)r_.i32(); // MinorVersion

            while (true) {
                uint8_t rt = r_.u8();
                if (rt == 11) break;      // MessageEnd
                dispatch_record(rt, /*report=*/true);
            }
        }
    }

private:
    void dispatch_record(uint8_t record_type, bool report) {
        switch (record_type) {
        case 1:  read_class_with_id(report); break;
        case 4:  read_class_with_members_and_types(/*system=*/true,  report); break;
        case 5:  read_class_with_members_and_types(/*system=*/false, report); break;
        case 6:  read_binary_object_string(report); break;
        case 7:  read_binary_array(report); break;
        case 8:  read_member_primitive_typed(report); break;
        case 9:  read_member_reference(report); break;
        case 10: read_object_null(report); break;
        case 12: read_binary_library(); break;
        case 13: read_object_null_multiple_256(report); break;
        case 14: read_object_null_multiple(report); break;
        case 15: read_array_single_primitive(report); break;
        case 16: read_array_single_object(report); break;
        case 17: read_array_single_string(report); break;
        default:
            throw ParseError("Unsupported NRBF record type " +
                             std::to_string(record_type) +
                             " at offset " + std::to_string(r_.pos() - 1));
        }
    }

    void read_binary_library() {
        int32_t id = r_.i32();
        std::string name = r_.lpstr();
        v_.library(id, name);
        libraries_[id] = std::move(name);
    }

    void read_class_with_members_and_types(bool system, bool report) {
        ClassDef def;
        def.system_class = system;
        def.object_id = r_.i32();
        def.name = r_.lpstr();
        int32_t mcount = r_.i32();
        def.members.resize(static_cast<size_t>(mcount));
        for (auto& m : def.members) m.name = r_.lpstr();
        for (auto& m : def.members) m.binary_type = static_cast<BinaryType>(r_.u8());
        for (auto& m : def.members) {
            switch (m.binary_type) {
            case BinaryType::Primitive:
            case BinaryType::PrimitiveArray:
                m.prim_type = static_cast<PrimitiveType>(r_.u8());
                break;
            case BinaryType::SystemClass:
                m.class_name = r_.lpstr();
                break;
            case BinaryType::Class:
                m.class_name = r_.lpstr();
                m.library_id = r_.i32();
                break;
            default: break;
            }
        }
        if (!system) {
            def.library_id = r_.i32();
            auto it = libraries_.find(def.library_id);
            if (it != libraries_.end()) def.library_name = it->second;
        }
        // Register before reading body so self-referential members work.
        const ClassDef* registered = &(classes_[def.object_id] = std::move(def));
        read_class_body(*registered, report);
    }

    void read_class_with_id(bool report) {
        int32_t obj_id = r_.i32();
        int32_t meta_id = r_.i32();
        auto it = classes_.find(meta_id);
        if (it == classes_.end()) {
            throw ParseError("ClassWithId references unknown metadata id " +
                             std::to_string(meta_id));
        }
        const ClassDef& base = it->second;
        // ClassWithId reuses the class shape from `base` but is its own
        // instance - we don't insert a new ClassDef.
        ClassDef alias = base;
        alias.object_id = obj_id;
        read_class_body(alias, report);
    }

    void read_class_body(const ClassDef& def, bool report) {
        bool descend = report ? v_.enter_instance(def.object_id, def) : false;
        for (const auto& m : def.members) {
            read_member_value(m, /*report=*/descend);
        }
        if (descend) v_.exit_instance(def.object_id, def);
    }

    // Read a single member value. For Primitive members the value is
    // inline (no record header byte). For everything else it's a record.
    void read_member_value(const ClassMember& m, bool report) {
        if (m.binary_type == BinaryType::Primitive) {
            Value v = read_primitive(r_, m.prim_type);
            if (report) v_.member(m, v);
            return;
        }
        uint8_t rt = r_.u8();
        // We need to surface a Value for the visitor's `member` callback
        // - synthesize one based on the inner record kind.
        Value v;
        switch (rt) {
        case 6: { // BinaryObjectString
            int32_t id = r_.i32();
            std::string_view sv = r_.lpstr_view();
            if (report) {
                v.kind = Value::Kind::ObjectRef;
                v.object_id = id;
                v.s.assign(sv);
                v_.member(m, v);
                v_.string_object(id, sv);
            }
            break;
        }
        case 9: { // MemberReference
            int32_t id = r_.i32();
            if (report) {
                v.kind = Value::Kind::ObjectRef;
                v.object_id = id;
                v_.member(m, v);
            }
            break;
        }
        case 10: { // ObjectNull
            if (report) {
                v.kind = Value::Kind::Null;
                v_.member(m, v);
            }
            break;
        }
        case 8: { // MemberPrimitiveTyped - primitive value inline, with type byte
            PrimitiveType pt = static_cast<PrimitiveType>(r_.u8());
            Value pv = read_primitive(r_, pt);
            if (report) v_.member(m, pv);
            break;
        }
        case 1: case 4: case 5: case 7:
        case 15: case 16: case 17: {
            // Nested class / array. Emit a placeholder ObjectRef so
            // the visitor sees a member callback; the inner record's
            // own callbacks carry the real id and payload.
            if (report) {
                v.kind = Value::Kind::ObjectRef;
                v.object_id = 0;
                v_.member(m, v);
            }
            dispatch_record(rt, report);
            break;
        }
        case 13: case 14: {
            // Run-length null sequences inside object arrays - but legal
            // in member position too if our class layout is wrong. Treat
            // each null as one member-null and consume accordingly.
            int32_t count = (rt == 13) ? int32_t(r_.u8()) : r_.i32();
            for (int32_t i = 0; i < count; ++i) {
                if (report) {
                    v.kind = Value::Kind::Null;
                    v_.member(m, v);
                }
            }
            break;
        }
        default:
            throw ParseError("Unexpected record type " +
                             std::to_string(rt) +
                             " as class member value at offset " +
                             std::to_string(r_.pos() - 1));
        }
    }

    void read_binary_object_string(bool report) {
        int32_t id = r_.i32();
        std::string_view s = r_.lpstr_view();
        if (report) v_.string_object(id, s);
    }

    void read_member_primitive_typed(bool report) {
        PrimitiveType pt = static_cast<PrimitiveType>(r_.u8());
        Value v = read_primitive(r_, pt);
        if (report) v_.primitive(pt, v);
    }

    void read_member_reference(bool report) {
        int32_t id = r_.i32();
        (void)report;
        (void)id;
        // No visitor hook needed; member references are surfaced via
        // class-member callbacks.
    }

    void read_object_null(bool report) {
        (void)report;
        // Surfaced through class-member context.
    }

    void read_object_null_multiple_256(bool report) {
        uint8_t n = r_.u8();
        pending_nulls_ = n;
        (void)report;
    }

    void read_object_null_multiple(bool report) {
        int32_t n = r_.i32();
        pending_nulls_ = n;
        (void)report;
    }

    void read_array_single_primitive(bool report) {
        int32_t id = r_.i32();
        int32_t len = r_.i32();
        PrimitiveType pt = static_cast<PrimitiveType>(r_.u8());
        bool descend = report ? v_.enter_primitive_array(id, pt, len) : false;
        if (descend) {
            for (int32_t i = 0; i < len; ++i) {
                Value v = read_primitive(r_, pt);
                v_.primitive_array_value(i, v);
            }
            v_.exit_primitive_array(id);
        } else {
            int sz = primitive_fixed_size(pt);
            if (sz > 0) {
                (void)r_.view(static_cast<size_t>(sz) *
                              static_cast<size_t>(len));
            } else {
                for (int32_t i = 0; i < len; ++i) {
                    (void)read_primitive(r_, pt);
                }
            }
        }
    }

    void read_array_single_string(bool report) {
        int32_t id = r_.i32();
        int32_t len = r_.i32();
        bool descend = report ? v_.enter_string_array(id, len) : false;
        for (int32_t i = 0; i < len; ++i) {
            uint8_t rt = r_.u8();
            if (rt == 6) {
                int32_t sid = r_.i32();
                std::string s = r_.lpstr();
                if (descend) v_.string_array_value(i, s);
                (void)sid;
            } else if (rt == 9) {
                (void)r_.i32();   // reference, ignored
            } else if (rt == 10) {
                // null element
            } else if (rt == 13) {
                uint8_t n = r_.u8();
                i += n - 1;
            } else if (rt == 14) {
                int32_t n = r_.i32();
                i += n - 1;
            } else {
                throw ParseError("Unexpected record in ArraySingleString: " +
                                 std::to_string(rt));
            }
        }
        if (descend) v_.exit_string_array(id);
    }

    void read_array_single_object(bool report) {
        int32_t id = r_.i32();
        int32_t len = r_.i32();
        bool descend = report ? v_.enter_object_array(id, len) : false;
        int32_t i = 0;
        while (i < len) {
            uint8_t rt = r_.u8();
            if (rt == 13) {
                uint8_t n = r_.u8();
                i += n;
                continue;
            }
            if (rt == 14) {
                int32_t n = r_.i32();
                i += n;
                continue;
            }
            dispatch_record(rt, descend);
            ++i;
        }
        if (descend) v_.exit_object_array(id);
    }

    // BinaryArray covers multi-dim and offset variants on top of the
    // ArraySingle* shape. Primitive payloads dispatch through
    // enter_primitive_array / primitive_array_value / exit; object
    // payloads through enter_object_array / per-record dispatch /
    // exit - same surface as the ArraySingle* records so consumers
    // don't have to special-case BinaryArray.
    void read_binary_array(bool report) {
        int32_t id = r_.i32();
        uint8_t array_type = r_.u8();  // 0..5 per spec 2.4.1.1
        int32_t rank = r_.i32();
        if (rank <= 0 || rank > 32) {
            throw ParseError("BinaryArray rank out of range");
        }
        int64_t total = 1;
        for (int32_t k = 0; k < rank; ++k) total *= r_.i32();
        // LowerBounds present for SingleOffset (3) / JaggedOffset (4) /
        // RectangularOffset (5).
        if (array_type == 3 || array_type == 4 || array_type == 5) {
            for (int32_t k = 0; k < rank; ++k) (void)r_.i32();
        }
        BinaryType bt = static_cast<BinaryType>(r_.u8());
        PrimitiveType pt = PrimitiveType::None;
        if (bt == BinaryType::Primitive || bt == BinaryType::PrimitiveArray) {
            pt = static_cast<PrimitiveType>(r_.u8());
        } else if (bt == BinaryType::SystemClass) {
            (void)r_.lpstr();
        } else if (bt == BinaryType::Class) {
            (void)r_.lpstr();
            (void)r_.i32();
        }
        if (bt == BinaryType::Primitive ||
            bt == BinaryType::PrimitiveArray) {
            const bool descend = report && rank == 1 &&
                v_.enter_primitive_array(id, pt,
                                         static_cast<int32_t>(total));
            int sz = primitive_fixed_size(pt);
            if (sz > 0) {
                auto bytes = r_.view(static_cast<size_t>(sz) *
                                     static_cast<size_t>(total));
                if (descend) {
                    const uint8_t* p =
                        reinterpret_cast<const uint8_t*>(bytes.data());
                    for (int64_t k = 0; k < total; ++k) {
                        Value v;
                        switch (pt) {
                        case PrimitiveType::Boolean:
                        case PrimitiveType::Byte:
                            v.kind = Value::Kind::UInt;
                            v.u = p[k * sz];
                            break;
                        case PrimitiveType::SByte:
                            v.kind = Value::Kind::Int;
                            v.i = static_cast<int8_t>(p[k * sz]);
                            break;
                        default: {
                            Reader rr(p + k * sz,
                                      static_cast<size_t>(sz));
                            v = read_primitive(rr, pt);
                            break;
                        }}
                        v_.primitive_array_value(
                            static_cast<int32_t>(k), v);
                    }
                }
            } else {
                for (int64_t k = 0; k < total; ++k) {
                    Value v = read_primitive(r_, pt);
                    if (descend) {
                        v_.primitive_array_value(
                            static_cast<int32_t>(k), v);
                    }
                }
            }
            if (descend) v_.exit_primitive_array(id);
            return;
        }
        // Object-bearing array. Inner records dispatch with the
        // parent's `report` flag, not false - otherwise nested
        // BinaryObjectString records (e.g. the value strings inside
        // a Dictionary<String, T> serialized as BinaryArray) get
        // silently dropped.
        const bool descend = report && rank == 1 &&
            v_.enter_object_array(id, static_cast<int32_t>(total));
        int64_t consumed = 0;
        while (consumed < total) {
            uint8_t rt = r_.u8();
            if (rt == 13) {
                uint8_t n = r_.u8();
                consumed += n;
                continue;
            }
            if (rt == 14) {
                int32_t n = r_.i32();
                consumed += n;
                continue;
            }
            dispatch_record(rt, descend);
            ++consumed;
        }
        if (descend) v_.exit_object_array(id);
    }

    Reader r_;
    Visitor& v_;
    std::unordered_map<int32_t, ClassDef>    classes_;
    std::unordered_map<int32_t, std::string> libraries_;
    int32_t pending_nulls_ = 0;
};

class DumpVisitor : public Visitor {
public:
    DumpVisitor(std::string& out, size_t max_str_len)
        : out_(out), max_str_(max_str_len) {}

    void library(int32_t id, std::string_view name) override {
        line("BinaryLibrary id=", id, " name=\"", std::string(name), "\"");
    }
    bool enter_instance(int32_t id, const ClassDef& d) override {
        line("Instance id=", id, " class=", d.name,
             d.library_name.empty() ? "" : (" [" + d.library_name + "]"),
             " {");
        ++depth_;
        return true;
    }
    void member(const ClassMember& m, const Value& v) override {
        std::string vs = value_str(v);
        line(m.name, " = ", vs);
    }
    void exit_instance(int32_t, const ClassDef&) override {
        --depth_; line("}");
    }
    void string_object(int32_t id, std::string_view s) override {
        line("String id=", id, " = ", quote(std::string(s)));
    }
    void primitive(PrimitiveType, const Value& v) override {
        line("Primitive = ", value_str(v));
    }
    bool enter_primitive_array(int32_t id, PrimitiveType,
                               int32_t len) override {
        line("PrimitiveArray id=", id, " len=", len,
             len < 32 ? " [" : " (truncated)");
        return len < 32;
    }
    void primitive_array_value(int32_t i, const Value& v) override {
        line("  [", i, "] ", value_str(v));
    }
    void exit_primitive_array(int32_t) override {
        line("]");
    }
    bool enter_object_array(int32_t id, int32_t len) override {
        line("ObjectArray id=", id, " len=", len, " [");
        ++depth_;
        return depth_ < 8;
    }
    void exit_object_array(int32_t) override {
        --depth_; line("]");
    }
    bool enter_string_array(int32_t id, int32_t len) override {
        line("StringArray id=", id, " len=", len);
        return true;
    }
    void string_array_value(int32_t i, std::string_view s) override {
        line("  [", i, "] ", quote(std::string(s)));
    }
    void exit_string_array(int32_t) override {}

private:
    template <class... A>
    void line(A&&... a) {
        for (int i = 0; i < depth_; ++i) out_ += "  ";
        ((out_ += stringify(std::forward<A>(a))), ...);
        out_ += '\n';
    }
    template <class T>
    static std::string stringify(T&& v) {
        if constexpr (std::is_arithmetic_v<std::decay_t<T>>) {
            return std::to_string(v);
        } else {
            return std::string(std::forward<T>(v));
        }
    }
    std::string value_str(const Value& v) {
        switch (v.kind) {
        case Value::Kind::Null:      return "null";
        case Value::Kind::Bool:      return v.i ? "true" : "false";
        case Value::Kind::Int:       return std::to_string(v.i);
        case Value::Kind::UInt:      return std::to_string(v.u);
        case Value::Kind::Float:     return std::to_string(v.f32);
        case Value::Kind::Double:    return std::to_string(v.f64);
        case Value::Kind::String:    return quote(v.s);
        case Value::Kind::Decimal:   return "decimal(" + v.s + ")";
        case Value::Kind::DateTime:  return "datetime(0x" + hex(v.u) + ")";
        case Value::Kind::TimeSpan:  return "timespan(" + std::to_string(v.i) + ")";
        case Value::Kind::ObjectRef: return "->id" + std::to_string(v.object_id) +
                                            (v.s.empty() ? "" : (" =" + quote(v.s)));
        }
        return "?";
    }
    std::string quote(std::string s) {
        if (s.size() > max_str_) s = s.substr(0, max_str_) + "...";
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') s[i] = ' ';
        }
        return '"' + s + '"';
    }
    static std::string hex(uint64_t v) {
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return buf;
    }
    std::string& out_;
    size_t max_str_;
    int depth_ = 0;
};

}  // namespace

void parse(std::string_view bytes, Visitor& v) {
    NRBF_ZONE_N("nrbf::parse");
    if (bytes.empty()) throw ParseError("Empty NRBF stream");
    Parser p(reinterpret_cast<const uint8_t*>(bytes.data()),
             bytes.size(), v);
    p.run();
}

void dump(std::string_view bytes, std::string& out, size_t max_str_len) {
    DumpVisitor dv(out, max_str_len);
    parse(bytes, dv);
}

}  // namespace nrbf
