// Copyright (c) 2023 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office_singleton.h"
#include "base/at_exit.h"
#include "base/test/gtest_util.h"
#include "gtest/gtest.h"
#include "office/test/office_unittest_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace electron::office {

TEST(OfficeSingletonTest, Basic) {
  ScopedEnvironmentVariables env_overrides;

  {
    base::ShadowingAtExitManager sem;
    EXPECT_FALSE(OfficeSingleton::IsValid());
    OfficeSingleton* instance = OfficeSingleton::GetInstance();
    EXPECT_NE(nullptr, instance);
    EXPECT_TRUE(OfficeSingleton::IsValid());

    EXPECT_NE(nullptr, instance->GetOffice());
  }

  EXPECT_FALSE(OfficeSingleton::IsValid());
}

TEST(OfficeSingletonDeathTest, MissingFontConfig) {
  // LOK is noisy and we know it's going to crash
  const base::ScopedEnvironmentVariableOverride sal_log("SAL_LOG", "-WARN");

  {
    base::ShadowingAtExitManager sem;
    EXPECT_FALSE(OfficeSingleton::IsValid());
    OfficeSingleton* instance = nullptr;
    EXPECT_DEATH({ instance = OfficeSingleton::GetInstance(); }, "Fontconfig error");
    EXPECT_EQ(nullptr, instance);
    EXPECT_FALSE(OfficeSingleton::IsValid());
  }

  EXPECT_FALSE(OfficeSingleton::IsValid());
}
}  // namespace electron::office
