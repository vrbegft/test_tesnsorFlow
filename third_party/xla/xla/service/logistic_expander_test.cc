/* Copyright 2020 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/logistic_expander.h"

#include <memory>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/service/hlo_creation_utils.h"
#include "xla/service/hlo_parser.h"
#include "xla/service/hlo_pass_fix.h"
#include "xla/service/hlo_pass_pipeline.h"
#include "xla/service/pattern_matcher.h"
#include "xla/service/pattern_matcher_gmock.h"
#include "xla/service/shape_inference.h"
#include "xla/shape_util.h"
#include "xla/test.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/types.h"
#include "xla/window_util.h"
#include "xla/xla_data.pb.h"
#include "tsl/lib/core/status_test_util.h"

namespace xla {
namespace {

namespace m = match;

class LogisticExpanderTest : public HloTestBase {};


// option is enabled.
TEST_F(LogisticExpanderTest, ExpandWith) {
  const char* kModuleStr = R"(
    HloModule m
    test {
      p = f32[2,3] parameter(0)
      ROOT r = f32[2,3] logistic(p)
    }
  )";
  TF_ASSERT_OK_AND_ASSIGN(auto m, ParseAndReturnVerifiedModule(kModuleStr));

  auto computation = m->entry_computation();
  HloInstruction* root = computation->root_instruction();
  EXPECT_EQ(root->opcode(), HloOpcode::kLogistic);
  LogisticExpander logistic_expander;
  ASSERT_TRUE(logistic_expander.Run(m.get()).value());
  root = computation->root_instruction();
  EXPECT_THAT(m->entry_computation()->root_instruction(),
              GmockMatch(m::Divide(
                  m::Broadcast(m::ConstantScalar(1.0)),
                  m::AddAnyOrder(m::Broadcast(m::ConstantScalar(1.0)),
                                 m::Exp(m::Negate(m::Parameter(0)))))));
}

}  // namespace
}  // namespace xla
