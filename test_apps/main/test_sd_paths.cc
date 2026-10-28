// test_sd_paths.cc — SD 路径安全与素材文件名纯逻辑单测
#include "lucky_test.h"
#include <string>
#include "storage/sd_paths.h"

TEST(SdPaths, NormalizeRejectsUnsafe) {
    EXPECT_STREQ("", sd_paths::Normalize("").c_str());
    EXPECT_STREQ("", sd_paths::Normalize("abc").c_str());        // 必须绝对路径
    EXPECT_STREQ("", sd_paths::Normalize("/a/../b").c_str());    // 上跳
    EXPECT_STREQ("", sd_paths::Normalize("/a\\b").c_str());      // 反斜杠
    EXPECT_STREQ("", sd_paths::Normalize("/a: b").c_str());      // 非法字符
}

TEST(SdPaths, NormalizeFolds) {
    EXPECT_STREQ("/", sd_paths::Normalize("/").c_str());
    EXPECT_STREQ("/a/b", sd_paths::Normalize("/a//b").c_str());
    EXPECT_STREQ("/a/b", sd_paths::Normalize("/a/./b/").c_str());
    EXPECT_STREQ("/dir/文件.txt", sd_paths::Normalize("/dir/文件.txt").c_str());
}

TEST(SdPaths, FullUnderSdcard) {
    EXPECT_STREQ("/sdcard", sd_paths::Full("/").c_str());
    EXPECT_STREQ("/sdcard/record/a.dmx", sd_paths::Full("/record/a.dmx").c_str());
    EXPECT_STREQ("", sd_paths::Full("/../etc").c_str());
}

TEST(SdPaths, AssetAndSoundFiles) {
    EXPECT_STREQ("/sdcard/lucky/assets/0.lgo", sd_paths::AssetFile(0).c_str());
    EXPECT_STREQ("/sdcard/lucky/assets/47.lgo", sd_paths::AssetFile(47).c_str());
    EXPECT_STREQ("", sd_paths::AssetFile(-1).c_str());
    EXPECT_STREQ("", sd_paths::AssetFile(48).c_str());
    EXPECT_STREQ("/sdcard/lucky/sounds/win.pcm", sd_paths::SoundFile("win").c_str());
    EXPECT_STREQ("", sd_paths::SoundFile("bogus").c_str());
    EXPECT_STREQ("", sd_paths::SoundFile(nullptr).c_str());
}

TEST(SdPaths, ValidNames) {
    EXPECT_TRUE(sd_paths::ValidName("a.dmx"));
    EXPECT_TRUE(sd_paths::ValidName("现场音乐 01.mp3"));
    EXPECT_FALSE(sd_paths::ValidName(""));
    EXPECT_FALSE(sd_paths::ValidName(".."));
    EXPECT_FALSE(sd_paths::ValidName("a/b"));
    EXPECT_FALSE(sd_paths::ValidName(std::string(121, 'x')));
}
