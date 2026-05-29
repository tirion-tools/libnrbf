// SPDX-License-Identifier: MIT
// Unit tests for nrbf::MultiVisitor: each callback is invoked directly and
// we check every child saw it. A missed override after a new Visitor
// virtual shows up as an uncounted callback here.

#include "nrbf/nrbf.hpp"

#include <cstdio>

namespace {

struct CountingVisitor final : public nrbf::Visitor {
    int libraries = 0;
    int enters = 0;
    int members = 0;
    int exits = 0;
    int strings = 0;
    int primitives = 0;
    int enter_prim_arr = 0;
    int prim_arr_value = 0;
    int exit_prim_arr = 0;
    int enter_obj_arr = 0;
    int exit_obj_arr = 0;
    int enter_str_arr = 0;
    int str_arr_value = 0;
    int exit_str_arr = 0;
    bool enter_returns;

    explicit CountingVisitor(bool r) : enter_returns(r) {}

    void library(int32_t, std::string_view) override { ++libraries; }
    bool enter_instance(int32_t, const nrbf::ClassDef&) override {
        ++enters; return enter_returns;
    }
    void member(const nrbf::ClassMember&, const nrbf::Value&) override {
        ++members;
    }
    void exit_instance(int32_t, const nrbf::ClassDef&) override { ++exits; }
    void string_object(int32_t, std::string_view) override { ++strings; }
    void primitive(nrbf::PrimitiveType, const nrbf::Value&) override {
        ++primitives;
    }
    bool enter_primitive_array(int32_t, nrbf::PrimitiveType, int32_t) override {
        ++enter_prim_arr; return enter_returns;
    }
    void primitive_array_value(int32_t, const nrbf::Value&) override {
        ++prim_arr_value;
    }
    void exit_primitive_array(int32_t) override { ++exit_prim_arr; }
    bool enter_object_array(int32_t, int32_t) override {
        ++enter_obj_arr; return enter_returns;
    }
    void exit_object_array(int32_t) override { ++exit_obj_arr; }
    bool enter_string_array(int32_t, int32_t) override {
        ++enter_str_arr; return enter_returns;
    }
    void string_array_value(int32_t, std::string_view) override {
        ++str_arr_value;
    }
    void exit_string_array(int32_t) override { ++exit_str_arr; }
};

int fails = 0;

#define EXPECT(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr); \
        ++fails; \
    } \
} while (0)

}  // namespace

int main() {
    nrbf::ClassDef    def;
    nrbf::ClassMember mem;
    nrbf::Value       val;

    // Two children, one says "descend" and one says "don't" – OR is true.
    {
        CountingVisitor a(true);
        CountingVisitor b(false);
        nrbf::MultiVisitor m;
        m.add(a);
        m.add(b);

        m.library(1, "L");
        EXPECT(a.libraries == 1);
        EXPECT(b.libraries == 1);

        EXPECT(m.enter_instance(7, def) == true);
        EXPECT(a.enters == 1);
        EXPECT(b.enters == 1);

        m.member(mem, val);
        EXPECT(a.members == 1);
        EXPECT(b.members == 1);

        m.exit_instance(7, def);
        EXPECT(a.exits == 1);
        EXPECT(b.exits == 1);

        m.string_object(8, "s");
        EXPECT(a.strings == 1);
        EXPECT(b.strings == 1);

        m.primitive(nrbf::PrimitiveType::Int32, val);
        EXPECT(a.primitives == 1);
        EXPECT(b.primitives == 1);

        EXPECT(m.enter_primitive_array(9, nrbf::PrimitiveType::Int32, 3) == true);
        m.primitive_array_value(0, val);
        m.exit_primitive_array(9);
        EXPECT(a.enter_prim_arr == 1 && a.prim_arr_value == 1 && a.exit_prim_arr == 1);
        EXPECT(b.enter_prim_arr == 1 && b.prim_arr_value == 1 && b.exit_prim_arr == 1);

        EXPECT(m.enter_object_array(10, 2) == true);
        m.exit_object_array(10);
        EXPECT(a.enter_obj_arr == 1 && a.exit_obj_arr == 1);
        EXPECT(b.enter_obj_arr == 1 && b.exit_obj_arr == 1);

        EXPECT(m.enter_string_array(11, 2) == true);
        m.string_array_value(0, "x");
        m.exit_string_array(11);
        EXPECT(a.enter_str_arr == 1 && a.str_arr_value == 1 && a.exit_str_arr == 1);
        EXPECT(b.enter_str_arr == 1 && b.str_arr_value == 1 && b.exit_str_arr == 1);
    }

    // Both children say "no descend" – OR is false.
    {
        CountingVisitor c(false);
        CountingVisitor d(false);
        nrbf::MultiVisitor m;
        m.add(c);
        m.add(d);
        EXPECT(m.enter_instance(1, def) == false);
        EXPECT(m.enter_primitive_array(1, nrbf::PrimitiveType::Int32, 1) == false);
        EXPECT(m.enter_object_array(1, 1) == false);
        EXPECT(m.enter_string_array(1, 1) == false);
    }

    // No children – callbacks are safe, enter_* return false.
    {
        nrbf::MultiVisitor m;
        m.library(1, "x");
        EXPECT(m.enter_instance(1, def) == false);
        EXPECT(m.enter_primitive_array(1, nrbf::PrimitiveType::Int32, 1) == false);
        EXPECT(m.enter_object_array(1, 1) == false);
        EXPECT(m.enter_string_array(1, 1) == false);
    }

    if (fails == 0) {
        std::puts("OK");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", fails);
    return 1;
}
