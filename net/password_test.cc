// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fydeos-vnc/net/password.h"

#include <gtest/gtest.h>

namespace fydeos::vnc::net {

TEST(PasswordTest, HashThenVerifyRoundTrips) {
  auto hash = HashPassword("hunter2");
  ASSERT_TRUE(hash.has_value());
  EXPECT_TRUE(hash->starts_with("$2b$"));
  EXPECT_TRUE(VerifyPassword("hunter2", *hash));
  EXPECT_FALSE(VerifyPassword("hunter3", *hash));
}

TEST(PasswordTest, EmptyPlaintextRejected) {
  EXPECT_FALSE(HashPassword("").has_value());
  EXPECT_FALSE(VerifyPassword("", "$2b$12$abcdefghijklmnopqrstuv"));
  EXPECT_FALSE(VerifyPassword("anything", ""));
}

TEST(PasswordTest, DifferentSaltsProduceDifferentHashes) {
  auto a = HashPassword("samepassword");
  auto b = HashPassword("samepassword");
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_NE(*a, *b);
  EXPECT_TRUE(VerifyPassword("samepassword", *a));
  EXPECT_TRUE(VerifyPassword("samepassword", *b));
}

}  // namespace fydeos::vnc::net
