// test_sd_name_codec.cc — SD 文件名 UTF-8↔GBK 纯逻辑单测
#include "lucky_test.h"
#include <string>
#include "storage/sd_name_codec.h"

TEST(SdNameCodec, AsciiPassThrough) {
    EXPECT_STREQ("hello.txt", sd_name_codec::ToFs("hello.txt").c_str());
    EXPECT_STREQ("hello.txt", sd_name_codec::FromFs("hello.txt").c_str());
}

TEST(SdNameCodec, ChineseRoundTrip) {
    const std::string utf8 = "现场音乐_01.mp3";
    const std::string gbk = sd_name_codec::ToFs(utf8);
    EXPECT_TRUE(gbk.size() < utf8.size());        // GBK 两字节/汉字
    EXPECT_STREQ(utf8.c_str(), sd_name_codec::FromFs(gbk).c_str());
}

TEST(SdNameCodec, LongChineseNameRoundTrip) {
    const std::string utf8 = "超长中文目录名称用于验证查询缓冲是否会截断路径";
    EXPECT_STREQ(utf8.c_str(), sd_name_codec::FromFs(sd_name_codec::ToFs(utf8)).c_str());
}

TEST(SdNameCodec, InvalidGbkBecomesQuestion) {
    EXPECT_STREQ("?", sd_name_codec::FromFs("\xFF").c_str());
    EXPECT_STREQ("a?", sd_name_codec::FromFs(std::string("a\xFF", 2)).c_str());
}

TEST(SdNameCodec, UnmappableUtf8BecomesQuestion) {
    // Emoji 不在 GBK 表内 → '?'
    EXPECT_STREQ("?", sd_name_codec::ToFs("\xF0\x9F\x98\x80").c_str());
}
