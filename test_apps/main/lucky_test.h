// lucky_test.h — Googletest 风格 → IDF 片上 unity 的兼容层
//
// 计划文档中的 62 个测试用例按 Googletest 风格书写（TEST/EXPECT_EQ/...），
// 而 ESP-IDF 片上 unity 只提供 TEST_CASE + TEST_ASSERT_*。
// 本头文件做最小桥接，使计划的测试代码可以近乎原样落进 test_apps。
//
// ⚠️ 两点差异：
// 1. unity 的断言全部是"失败即中止"（相当于 gtest 的 ASSERT_*）；
//    EXPECT_* 的"继续执行"语义不可用——对嵌入式测试可接受。
// 2. 不支持 `<< "消息"` 流式附加说明；需要说明时用 TEST_ASSERT_EQUAL_MESSAGE。
#ifndef _LUCKY_TEST_H_
#define _LUCKY_TEST_H_

#include <string>
#include "unity.h"

// TEST(Group, Name) → TEST_CASE("Group.Name", "[group]")
#define TEST(group, name) \
    static void TEST_##group##_##name##_body(); \
    TEST_CASE(#group "." #name, "[" #group "]") { TEST_##group##_##name##_body(); } \
    static void TEST_##group##_##name##_body()

// ---- 相等断言：重载分派（std::string 走字符串比较，其余走整型） ----
inline void LuckyExpectEq(const std::string& a, const std::string& b) {
    TEST_ASSERT_EQUAL_STRING(b.c_str(), a.c_str());
}
inline void LuckyExpectEq(const std::string& a, const char* b) {
    TEST_ASSERT_EQUAL_STRING(b, a.c_str());
}
inline void LuckyExpectEq(const char* a, const std::string& b) {
    TEST_ASSERT_EQUAL_STRING(b.c_str(), a);
}
template <typename A, typename B>
inline void LuckyExpectEq(const A& a, const B& b) {
    TEST_ASSERT_EQUAL((UNITY_INT)(b), (UNITY_INT)(a));
}

#define EXPECT_EQ(a, b) LuckyExpectEq((a), (b))
#define ASSERT_EQ(a, b) LuckyExpectEq((a), (b))
#define EXPECT_NE(a, b)  TEST_ASSERT_NOT_EQUAL((UNITY_INT)(b), (UNITY_INT)(a))
#define ASSERT_NE(a, b)  TEST_ASSERT_NOT_EQUAL((UNITY_INT)(b), (UNITY_INT)(a))
#define EXPECT_LT(a, b)  TEST_ASSERT_LESS_THAN((b), (a))
#define EXPECT_LE(a, b)  TEST_ASSERT_LESS_OR_EQUAL((b), (a))
#define EXPECT_GT(a, b)  TEST_ASSERT_GREATER_THAN((b), (a))
#define EXPECT_GE(a, b)  TEST_ASSERT_GREATER_OR_EQUAL((b), (a))
#define ASSERT_LT(a, b)  TEST_ASSERT_LESS_THAN((b), (a))
#define ASSERT_LE(a, b)  TEST_ASSERT_LESS_OR_EQUAL((b), (a))
#define ASSERT_GT(a, b)  TEST_ASSERT_GREATER_THAN((b), (a))
#define ASSERT_GE(a, b)  TEST_ASSERT_GREATER_OR_EQUAL((b), (a))

// ---- 布尔 ----
#define EXPECT_TRUE(x)   TEST_ASSERT_TRUE((x))
#define EXPECT_FALSE(x)  TEST_ASSERT_FALSE((x))
#define ASSERT_TRUE(x)   TEST_ASSERT_TRUE((x))
#define ASSERT_FALSE(x)  TEST_ASSERT_FALSE((x))

// ---- 浮点 ----
#define EXPECT_NEAR(a, b, eps)   TEST_ASSERT_DOUBLE_WITHIN((eps), (b), (a))
#define ASSERT_NEAR(a, b, eps)   TEST_ASSERT_DOUBLE_WITHIN((eps), (b), (a))
#define EXPECT_DOUBLE_EQ(a, b)   TEST_ASSERT_EQUAL_DOUBLE((b), (a))

// ---- 类型化 ----
#define EXPECT_STREQ(a, b)       TEST_ASSERT_EQUAL_STRING((a), (b))   // gtest 序：expected 在前

#endif  // _LUCKY_TEST_H_
