//===- unittests/Core/DepsBuildEngineTest.cpp -----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/Core/BuildEngine.h"

#include "llbuild/Basic/ExecutionQueue.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/Core/BuildDB.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"

#include "gtest/gtest.h"

#include <unordered_map>
#include <vector>
#include <thread>

using namespace llbuild;
using namespace llbuild::core;

namespace {

class SimpleBuildEngineDelegate : public core::BuildEngineDelegate, public basic::ExecutionQueueDelegate {
  virtual std::unique_ptr<core::Rule> lookupRule(const core::KeyType& key) override {
    // We never expect dynamic rule lookup.
    fprintf(stderr, "error: unexpected rule lookup for \"%s\"\n",
            key.c_str());
    abort();
    return nullptr;
  }

  virtual void cycleDetected(const std::vector<core::Rule*>& items) override {
    // We never expect a cycle.
    fprintf(stderr, "error: cycle\n");
    abort();
  }

  virtual void error(const Twine& message) override {
    fprintf(stderr, "error: %s\n", message.str().c_str());
    abort();
  }

  void processStarted(basic::ProcessContext*, basic::ProcessHandle, llbuild_pid_t) override { }
  void processHadError(basic::ProcessContext*, basic::ProcessHandle, const Twine&) override { }
  void processHadOutput(basic::ProcessContext*, basic::ProcessHandle, StringRef) override { }
  void processFinished(basic::ProcessContext*, basic::ProcessHandle, const basic::ProcessResult&) override { }
  void queueJobStarted(basic::JobDescriptor*) override { }
  void queueJobFinished(basic::JobDescriptor*) override { }

  std::unique_ptr<basic::ExecutionQueue> createExecutionQueue() override {
    return createSerialQueue(*this, nullptr);
  }
};

static int32_t intFromValue(const core::ValueType& value) {
  assert(value.size() == 4);
  return ((value[0] << 0) |
          (value[1] << 8) |
          (value[2] << 16) |
          (value[3] << 24));
}
static core::ValueType intToValue(int32_t value) {
  std::vector<uint8_t> result(4);
  result[0] = (value >> 0) & 0xFF;
  result[1] = (value >> 8) & 0xFF;
  result[2] = (value >> 16) & 0xFF;
  result[3] = (value >> 24) & 0xFF;
  return result;
}

// Simple task implementation which takes a fixed set of dependencies, evaluates
// them all, and then provides the output.
class SimpleTask : public Task {
public:
  typedef std::function<int(const std::vector<int>&)> ComputeFnType;

private:
  std::vector<KeyType> inputs;
  std::vector<int> inputValues;
  ComputeFnType compute;

public:
  SimpleTask(const std::vector<KeyType>& inputs, ComputeFnType compute)
    : inputs(inputs), compute(compute)
  {
    inputValues.resize(inputs.size());
  }

  virtual void start(TaskInterface ti) override {
    // Request all of the inputs.
    for (int i = 0, e = inputs.size(); i != e; ++i) {
      ti.request(inputs[i], i);
    }
  }

  virtual void provideValue(TaskInterface, uintptr_t inputID,
                            const KeyType& key, const ValueType& value) override {
    // Update the input values.
    assert(inputID < inputValues.size());
    inputValues[inputID] = intFromValue(value);
  }

  virtual void inputsAvailable(TaskInterface ti) override {
    ti.complete(intToValue(compute(inputValues)));
  }
};

// Helper function for creating a simple action.
typedef std::function<Task*(BuildEngine&)> ActionFn;

class SimpleRule: public Rule {
public:
    typedef std::function<bool(const ValueType& value)> ValidFnType;

private:
    SimpleTask::ComputeFnType compute;
    std::vector<KeyType> inputs;
    ValidFnType valid;
public:
    SimpleRule(const KeyType& key, const std::vector<KeyType>& inputs,
               SimpleTask::ComputeFnType compute, ValidFnType valid = nullptr)
        : Rule(key), compute(compute), inputs(inputs), valid(valid) { }

    Task* createTask(BuildEngine&) override { return new SimpleTask(inputs, compute); }

    bool isResultValid(BuildEngine&, const ValueType& value) override {
        if (!valid) return true;
        return valid(value);
    }
};

// Test for a tricky case involving concurrent dependency scanning.
//
// The problem that this test is checking for is that we don't immediately start
// running rules we have *scanned* as part of an input dependency, before they
// have actually been requested. For example, if a rule requests something like
// the contents of a directory, then requests something based on each file in
// the directory, we don't want to possibly run a rule for a file which was
// previously present (and thus recorded as an input dependency) but which is no
// longer present.
TEST(DepsBuildEngineTest, BogusConcurrentDepScan) {
  std::vector<KeyType> builtKeys;
  SimpleBuildEngineDelegate delegate;
  core::BuildEngine engine(delegate);
  
  // This models a rule which retrieves some dynamic content (like a directory
  // list).
  //
  // We need to model a separate input to trigger the continued scanning
  // behavior (so that when "output" is considering its "dir-list" input, it
  // isn't immediately obvious it needs to run).
  int dirListValue = 2 * 3;
  engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
      "dir-list-input", {}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("dir-list-input");
          return dirListValue; },
      [&](const ValueType&) {
        // Always rebuild
        return false;
      } )));
  engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
      "dir-list", { "dir-list-input" }, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("dir-list");
          assert(inputs.size() == 1);
          return inputs[0]; })));

  // These are the rules for individual discovered "files".
  engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
      "input-2", {}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("input-2");
          return 5; },
      [&](const ValueType&) {
        // Always rebuild
        return false;
      } )));
  engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
      "input-3", {}, [&] (const std::vector<int>& inputs) {
          builtKeys.push_back("input-3");
          return 7; },
      [&](const ValueType&) {
        // Always rebuild
        return false;
      } )));

  // This models a rule which uses the dynamic content to drive some other
  // action (compiling files).
  class DynamicTask : public Task {
    int result = 1;

  public:
    virtual void start(TaskInterface ti) override {
      // Request the known input.
      ti.request("dir-list", 0);
    }

    virtual void provideValue(TaskInterface ti, uintptr_t inputID,
                              const KeyType& key, const ValueType& value) override {
      int N = intFromValue(value);
      result *= N;

      // If this is the initial input, use it to derive
      // the subsequent inputs.
      if (inputID == 0) {
        if (N == 2) {
          ti.request("input-2", 1);
        } else if (N == 3) {
          ti.request("input-3", 1);
        } else {
          assert(N == 2 * 3);
          ti.request("input-2", 1);
          ti.request("input-3", 1);
        }
      }
    }

    virtual void inputsAvailable(TaskInterface ti) override {
      ti.complete(intToValue(result));
    }
  };

  class DynamicRule: public Rule {
    std::vector<KeyType>& builtKeys;
  public:
    DynamicRule(const KeyType& key, std::vector<KeyType>& builtKeys)
      : Rule(key), builtKeys(builtKeys) { }
    Task* createTask(BuildEngine&) override {
      builtKeys.push_back(key);
      return new DynamicTask();
    }
    bool isResultValid(BuildEngine&, const ValueType&) override { return true; }
  };
  engine.addRule(std::unique_ptr<core::Rule>(new DynamicRule("output", builtKeys)));

  // Build the first result.
  EXPECT_EQ(2 * 3 * 5 * 7, intFromValue(engine.build("output")));
  EXPECT_EQ(5U, builtKeys.size());
  EXPECT_EQ(KeyType("output"), builtKeys[0]);
  EXPECT_EQ(KeyType("dir-list-input"), builtKeys[1]);
  EXPECT_EQ(KeyType("dir-list"), builtKeys[2]);
  EXPECT_EQ(KeyType("input-2"), builtKeys[3]);
  EXPECT_EQ(KeyType("input-3"), builtKeys[4]);

  // Now change the dynamic contents and rebuild.
  builtKeys.clear();
  // Suppress a static analyzer false positive (rdar://problem/22165179).
#ifndef __clang_analyzer__
  dirListValue = 3;
#endif
  EXPECT_EQ(3 * 7, intFromValue(engine.build("output")));
  EXPECT_EQ(4U, builtKeys.size());
  EXPECT_EQ(KeyType("dir-list-input"), builtKeys[0]);
  EXPECT_EQ(KeyType("dir-list"), builtKeys[1]);
  EXPECT_EQ(KeyType("output"), builtKeys[2]);
  EXPECT_EQ(KeyType("input-3"), builtKeys[3]);
}

TEST(DepsBuildEngineTest, KeysWithNull) {
  // Check build engine support for keys with embedded null characters.
  std::vector<std::string> builtKeys;
  SimpleBuildEngineDelegate delegate;

  // Create a temporary file.
  llvm::SmallString<256> dbPath;
  auto ec = llvm::sys::fs::createTemporaryFile("build", "db", dbPath);
  EXPECT_EQ(bool(ec), false);

  fprintf(stderr, "using db: %s\n", dbPath.c_str());
  for (int iteration = 0; iteration < 2; ++iteration) {
    fprintf(stderr, "iteration: %d\n", iteration);
    
    core::BuildEngine engine(delegate);

    // Attach the database.
    {
      std::string error;
      auto db = createSQLiteBuildDB(dbPath, 1, /* recreateUnmatchedVersion = */ true, &error);
      EXPECT_EQ(bool(db), true);
      if (!db) {
        fprintf(stderr, "unable to open database: %s\n", error.c_str());
        return;
      }
      engine.attachDB(std::move(db), &error);
    }

    std::string inputA{"i\0A", 3};
    std::string inputB{"i\0B", 3};
    engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
        inputA, {}, [&] (const std::vector<int>& inputs) {
            builtKeys.push_back(inputA);
            return 2; })));
    engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
        inputB, {}, [&] (const std::vector<int>& inputs) {
            builtKeys.push_back(inputB);
            return 3; })));
    engine.addRule(std::unique_ptr<core::Rule>(new SimpleRule(
        "output", { inputA, inputB }, [&] (const std::vector<int>& inputs) {
            assert(inputs.size() == 2);
            builtKeys.push_back("output");
            return inputs[0] * inputs[1]; })));

    // Run the build.
    builtKeys.clear();
    EXPECT_EQ(2 * 3, intFromValue(engine.build("output")));
    if (iteration == 0) {
      EXPECT_EQ(3U, builtKeys.size());
      EXPECT_EQ(inputA, builtKeys[0]);
      EXPECT_EQ(inputB, builtKeys[1]);
      EXPECT_EQ("output", builtKeys[2]);
    } else {
      EXPECT_EQ(0U, builtKeys.size());
    }
  }

  ec = llvm::sys::fs::remove(dbPath.str());
  EXPECT_EQ(bool(ec), false);
}

}
