/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: SMAP5.0 测试代码
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/nodemask.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#include "tracking_manage.h"
#include "iomem.h"
#include "pid_ioctl.h"
#include "ham_migration.h"

using namespace std;

extern "C" char *qemu_name;
extern "C" int node_modes[SMAP_MAX_NUMNODES];

class TrackingManageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        cout << "[Phase SetUp Begin]" << endl;
        cout << "[Phase SetUp End]" << endl;
    }
    void TearDown() override
    {
        cout << "[Phase TearDown Begin]" << endl;
        GlobalMockObject::verify();
        cout << "[Phase TearDown End]" << endl;
    }
};

extern "C" int refresh_remote_ram(void);
extern "C" int iterate_obmm_dev(void);
extern "C" void destroy_workqueue(struct workqueue_struct *wq);
extern "C" bool cancel_delayed_work_sync(struct delayed_work *dwork);
extern "C" bool queue_delayed_work(struct workqueue_struct *wq,
    struct delayed_work *dwork,
    unsigned long delay);
extern "C" void ham_exit(void);

extern "C" void release_remote_ram(void);
extern "C" void reset_acpi_mem(void);
extern "C" void exit_migrate(void);
extern "C" void smap_dev_exit(void);
extern "C" void smap_debugfs_mod_exit(void);
extern "C" void resource(void);
TEST_F(TrackingManageTest, Resource)
{
    MOCKER(cancel_delayed_work_sync).stubs().will(returnValue(true));
    MOCKER(release_remote_ram).stubs().will(ignoreReturnValue());
    MOCKER(reset_acpi_mem).stubs().will(ignoreReturnValue());
    MOCKER(destroy_workqueue).stubs().will(ignoreReturnValue());
    MOCKER(exit_migrate).stubs().will(ignoreReturnValue());
    MOCKER(smap_dev_exit).stubs().will(ignoreReturnValue());
    MOCKER(smap_debugfs_mod_exit).stubs().will(ignoreReturnValue());
    MOCKER(ham_exit).stubs().will(ignoreReturnValue());
    resource();
}

extern "C" int is_qemu_name_valid(void);
TEST_F(TrackingManageTest, IsQemuNameValid)
{
    int ret;

    qemu_name = "\0";
    ret = is_qemu_name_valid();
    EXPECT_EQ(-EINVAL, ret);

    qemu_name = "qemu-kvm";
    ret = is_qemu_name_valid();
    EXPECT_EQ(0, ret);
}

extern "C" int is_smap_args_valid(void);
TEST_F(TrackingManageTest, IsArgsVaild)
{
    int ret;

    smap_pgsize = 10;
    ret = is_smap_args_valid();
    EXPECT_EQ(-EINVAL, ret);

    smap_pgsize = 0;
    smap_mode = 9;
    ret = is_smap_args_valid();
    EXPECT_EQ(-EINVAL, ret);

    smap_pgsize = 0;
    smap_mode = 0;
    node_modes[0] = 100;
    ret = is_smap_args_valid();
    EXPECT_EQ(-EINVAL, ret);

    for (int i = 0; i < 10; i++) {
        node_modes[i] = i;
    }

    MOCKER(is_qemu_name_valid).stubs().will(returnValue(-EINVAL));
    ret = is_smap_args_valid();
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(is_qemu_name_valid).stubs().will(returnValue(0));
    ret = is_smap_args_valid();
    EXPECT_EQ(0, ret);
}

extern "C" struct list_head migrate_back_task_list;
extern "C" void migrate_back_work_func(struct work_struct *work);
TEST_F(TrackingManageTest, MigrateBackWorkFunc)
{
    struct work_struct work;
    struct migrate_back_task task;
    struct migrate_back_subtask subtask;

    task.subtask_cnt = 10;
    task.status = MB_TASK_WAITING;
    subtask.status = MB_SUBTASK_WAITING;
    subtask.isolated_flag = 0;
    subtask.pa_start = 0;
    subtask.pa_end = 0x100;
    INIT_LIST_HEAD(&migrate_back_task_list);
    INIT_LIST_HEAD(&task.subtask);
    list_add(&task.task_node, &migrate_back_task_list);
    MOCKER(is_smap_pg_huge).stubs().will(returnValue(true));
    MOCKER(smap_handle_migrate_back_subtask).stubs();
    migrate_back_work_func(&work);

    list_del(&task.task_node);
}

TEST_F(TrackingManageTest, MigrateBackWorkFunc4kAndQueueNextRound)
{
    struct work_struct work = {};
    struct migrate_back_task task = {};
    struct migrate_back_subtask subtask = {};

    task.subtask_cnt = 2;
    task.status = MB_TASK_WAITING;
    subtask.status = MB_SUBTASK_DONE;

    INIT_LIST_HEAD(&migrate_back_task_list);
    INIT_LIST_HEAD(&task.subtask);
    list_add(&subtask.task_list, &task.subtask);
    list_add(&task.task_node, &migrate_back_task_list);

    MOCKER(iterate_obmm_dev).stubs().will(returnValue(0));
    MOCKER(is_smap_pg_huge).stubs().will(returnValue(false));
    MOCKER(smap_handle_migrate_back_subtask_4k).stubs().will(ignoreReturnValue());
    MOCKER(queue_delayed_work).stubs().will(returnValue(true));

    migrate_back_work_func(&work);

    EXPECT_EQ(MB_TASK_DONE, task.status);
    EXPECT_EQ(HUNDRED, task.progress);
    list_del(&subtask.task_list);
    list_del(&task.task_node);
}

TEST_F(TrackingManageTest, MigrateBackWorkFuncMarksTaskError)
{
    struct work_struct work = {};
    struct migrate_back_task task = {};
    struct migrate_back_subtask subtask = {};

    task.subtask_cnt = 1;
    task.status = MB_TASK_WAITING;
    subtask.status = MB_SUBTASK_ERR;

    INIT_LIST_HEAD(&migrate_back_task_list);
    INIT_LIST_HEAD(&task.subtask);
    list_add(&subtask.task_list, &task.subtask);
    list_add(&task.task_node, &migrate_back_task_list);

    MOCKER(iterate_obmm_dev).stubs().will(returnValue(-1));
    MOCKER(is_smap_pg_huge).stubs().will(returnValue(true));
    MOCKER(smap_handle_migrate_back_subtask).stubs().will(ignoreReturnValue());
    MOCKER(queue_delayed_work).stubs().will(returnValue(true));

    migrate_back_work_func(&work);

    EXPECT_EQ(MB_TASK_ERR, task.status);
    EXPECT_EQ(HUNDRED, task.progress);
    list_del(&subtask.task_list);
    list_del(&task.task_node);
}

extern "C" int init_acpi_mem(void);
extern "C" struct workqueue_struct *migrate_back_wq;
extern "C" int __init tracking_init(void);
TEST_F(TrackingManageTest, TestTrackingInit)
{
    int ret;

    MOCKER(is_smap_args_valid).stubs().will(returnValue(-1));
    ret = tracking_init();
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    MOCKER(is_smap_args_valid).stubs().will(returnValue(0));
    MOCKER(smap_process_symbols).stubs().will(returnValue(-1));
    ret = tracking_init();
    EXPECT_EQ(-1, ret);
}

TEST_F(TrackingManageTest, TestTrackingInitAcpiAndRemoteRamFailure)
{
    int ret;
    struct ram_segment seg = {};

    INIT_LIST_HEAD(&remote_ram_list);
    smap_scene = NORMAL_SCENE;

    MOCKER(is_smap_args_valid).stubs().will(returnValue(0));
    MOCKER(smap_process_symbols).stubs().will(returnValue(0));
    MOCKER(init_acpi_mem).stubs().will(returnValue(-1));
    ret = tracking_init();
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    INIT_LIST_HEAD(&remote_ram_list);
    MOCKER(is_smap_args_valid).stubs().will(returnValue(0));
    MOCKER(smap_process_symbols).stubs().will(returnValue(0));
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(refresh_remote_ram).stubs().will(returnValue(0));
    MOCKER(release_remote_ram).stubs().will(ignoreReturnValue());
    MOCKER(reset_acpi_mem).stubs().will(ignoreReturnValue());
    ret = tracking_init();
    EXPECT_EQ(-EINVAL, ret);
    GlobalMockObject::verify();

    INIT_LIST_HEAD(&remote_ram_list);
    INIT_LIST_HEAD(&seg.node);
    list_add(&seg.node, &remote_ram_list);
    MOCKER(is_smap_args_valid).stubs().will(returnValue(0));
    MOCKER(smap_process_symbols).stubs().will(returnValue(0));
    MOCKER(init_acpi_mem).stubs().will(returnValue(0));
    MOCKER(refresh_remote_ram).stubs().will(returnValue(0));
    MOCKER(release_remote_ram).stubs().will(ignoreReturnValue());
    MOCKER(reset_acpi_mem).stubs().will(ignoreReturnValue());
    ret = tracking_init();
    EXPECT_EQ(-EAGAIN, ret);
    list_del(&seg.node);
}

extern "C" void resource(void);
extern "C" void __exit tracking_exit(void);
TEST_F(TrackingManageTest, TestTrackingExit)
{
    MOCKER(resource).stubs().will(ignoreReturnValue());
    tracking_exit();
}
