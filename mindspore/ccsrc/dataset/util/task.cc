/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "dataset/util/task.h"
#include "common/utils.h"
#include "dataset/util/task_manager.h"
#include "dataset/util/de_error.h"
#include "utils/log_adapter.h"

namespace mindspore {
namespace dataset {
thread_local Task *gMyTask = nullptr;

void Task::operator()() {
  gMyTask = this;
  std::stringstream ss;
  ss << this_thread::get_id();
  MS_LOG(INFO) << my_name_ << " Thread ID " << ss.str() << " Started.";
  try {
    // Previously there is a timing hole where the thread is spawn but hit error immediately before we can set
    // the TaskGroup pointer and register. We move the registration logic to here (after we spawn) so we can
    // get the thread id.
    TaskGroup *vg = MyTaskGroup();
    rc_ = vg->GetIntrpService()->Register(ss.str(), this);
    if (rc_.IsOk()) {
      // Now we can run the given task.
      rc_ = fnc_obj_();
    }
    // Some error codes are ignored, e.g. interrupt. Others we just shutdown the group.
    if (rc_.IsError() && !rc_.IsInterrupted()) {
      ShutdownGroup();
    }
  } catch (const std::bad_alloc &e) {
    rc_ = Status(StatusCode::kOutOfMemory, __LINE__, __FILE__, e.what());
    ShutdownGroup();
  } catch (const std::exception &e) {
    rc_ = Status(StatusCode::kUnexpectedError, __LINE__, __FILE__, e.what());
    ShutdownGroup();
  }
}

void Task::ShutdownGroup() {  // Wake up watch dog and shutdown the engine.
  {
    std::lock_guard<std::mutex> lk(mux_);
    caught_severe_exception_ = true;
  }
  TaskGroup *vg = MyTaskGroup();
  // If multiple threads hit severe errors in the same group. Keep the first one and
  // discard the rest.
  if (vg->rc_.IsOk()) {
    std::unique_lock<std::mutex> rcLock(vg->rc_mux_);
    // Check again after we get the lock
    if (vg->rc_.IsOk()) {
      vg->rc_ = rc_;
      rcLock.unlock();
      TaskManager::InterruptMaster(rc_);
      TaskManager::InterruptGroup(*gMyTask);
    }
  }
}

Status Task::GetTaskErrorIfAny() {
  std::lock_guard<std::mutex> lk(mux_);
  if (caught_severe_exception_) {
    return rc_;
  } else {
    return Status::OK();
  }
}

Task::Task(const std::string &myName, const std::function<Status()> &f)
    : my_name_(myName),
      rc_(),
      fnc_obj_(f),
      task_group_(nullptr),
      is_master_(false),
      running_(false),
      caught_severe_exception_(false) {
  IntrpResource::ResetIntrpState();
  wp_.ResetIntrpState();
  wp_.Clear();
}

Status Task::Run() {
  Status rc;
  if (running_ == false) {
    try {
      thrd_ = std::thread(std::ref(*this));
      id_ = thrd_.get_id();
      running_ = true;
      caught_severe_exception_ = false;
    } catch (const std::exception &e) {
      rc = Status(StatusCode::kUnexpectedError, __LINE__, __FILE__, e.what());
    }
  }
  return rc;
}

Status Task::Join() {
  if (running_) {
    try {
      thrd_.join();
      std::stringstream ss;
      ss << get_id();
      MS_LOG(INFO) << MyName() << " Thread ID " << ss.str() << " Stopped.";
      running_ = false;
      RETURN_IF_NOT_OK(wp_.Deregister());
      if (MyTaskGroup()) {
        RETURN_IF_NOT_OK(MyTaskGroup()->GetIntrpService()->Deregister(ss.str()));
      }
    } catch (const std::exception &e) {
      RETURN_STATUS_UNEXPECTED(e.what());
    }
  }
  return Status::OK();
}

TaskGroup *Task::MyTaskGroup() { return task_group_; }

void Task::set_task_group(TaskGroup *vg) { task_group_ = vg; }

Task::~Task() { task_group_ = nullptr; }
}  // namespace dataset
}  // namespace mindspore
