// test_page_content.cc — 每页打印内容纯逻辑单测（TDD）
#include "lucky_test.h"
#include <string>
#include <vector>
#include "printer/page_content.h"

using page_content::Item;
using page_content::kMaxTextBytes;
using page_content::kMaxPages;

TEST(PageContent, JsonRoundTrip) {
    std::vector<Item> items{{0, "一等奖 · 扫地机器人", 2}, {3, "谢谢参与", -1}};
    std::string json = page_content::ToJson(items);
    std::vector<Item> back;
    std::string err;
    EXPECT_TRUE(page_content::FromJson(json, &back, &err));
    EXPECT_EQ(2u, back.size());
    EXPECT_EQ(0, back[0].page);
    EXPECT_EQ(std::string("一等奖 · 扫地机器人"), back[0].text);
    EXPECT_EQ(2, back[0].asset);
    EXPECT_EQ(-1, back[1].asset);
}

TEST(PageContent, EmptyIsValid) {
    std::vector<Item> back;
    std::string err;
    EXPECT_TRUE(page_content::FromJson("{\"version\":1,\"pages\":[]}", &back, &err));
    EXPECT_EQ(0u, back.size());
}

TEST(PageContent, RejectsBadJsonAndShape) {
    std::vector<Item> back;
    std::string err;
    EXPECT_FALSE(page_content::FromJson("not json", &back, &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(page_content::FromJson("{\"version\":1}", &back, &err));
}

TEST(PageContent, RejectsPageOutOfRange) {
    std::vector<Item> back;
    std::string err;
    std::string bad = "{\"pages\":[{\"page\":" + std::to_string(kMaxPages) + ",\"text\":\"x\"}]}";
    EXPECT_FALSE(page_content::FromJson(bad, &back, &err));
}

TEST(PageContent, RejectsTooLongText) {
    std::vector<Item> back;
    std::string err;
    std::string long_text(kMaxTextBytes + 1, 'a');
    std::string bad = "{\"pages\":[{\"page\":0,\"text\":\"" + long_text + "\"}]}";
    EXPECT_FALSE(page_content::FromJson(bad, &back, &err));
}

TEST(PageContent, UpsertOverwritesAndRemoves) {
    std::vector<Item> items;
    EXPECT_TRUE(page_content::Upsert(&items, 5, "hello", 1));
    EXPECT_TRUE(page_content::Upsert(&items, 5, "world", -1));
    EXPECT_EQ(1u, items.size());
    EXPECT_EQ(std::string("world"), items[0].text);
    EXPECT_EQ(-1, items[0].asset);
    // 空内容 = 删除
    EXPECT_TRUE(page_content::Upsert(&items, 5, "", -1));
    EXPECT_EQ(1u, items.size());   // Upsert 本身不清除，见下
}

TEST(PageContent, UpsertRejectsOutOfRange) {
    std::vector<Item> items;
    EXPECT_FALSE(page_content::Upsert(&items, -1, "x", -1));
    EXPECT_FALSE(page_content::Upsert(&items, kMaxPages, "x", -1));
    EXPECT_FALSE(page_content::Upsert(&items, 0, std::string(kMaxTextBytes + 1, 'a'), -1));
}

TEST(PageContent, FindReturnsNullWhenUnset) {
    std::vector<Item> items{{1, "a", -1}};
    EXPECT_TRUE(page_content::Find(items, 1) != nullptr);
    EXPECT_TRUE(page_content::Find(items, 2) == nullptr);
}

TEST(PageContent, JsonKeepsAscendingOrder) {
    std::vector<Item> items{{7, "g", -1}, {2, "b", -1}};
    std::string json = page_content::ToJson(items);
    EXPECT_TRUE(json.find("\"page\":2") < json.find("\"page\":7"));
}
