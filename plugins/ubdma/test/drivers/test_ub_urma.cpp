/*
 * Description: urma test
 */
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <iostream>
#include "gtest/gtest.h"
#include "linux/compiler_attributes.h"
#include "linux/device.h"
#include "linux/dmaengine.h"
#include "mockcpp/ChainingMockHelper.h"
#include "mockcpp/mokc.h"
#include "stub_struct.h"
#include "urma.h"

using namespace std;
class TestUbUrma : public ::testing::Test {
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

ubcore_seg_cfg g_cfg = {};

ubcore_target_seg *ubcore_register_seg_stub(ubcore_device *dev, ubcore_seg_cfg *cfg, ubcore_udata *udata)
{
    g_cfg = *cfg;
    return nullptr;
}

constexpr int TEST_SEGMENT_EID_INDEX = 100;
constexpr int TEST_SEGMENT_VA = 0x1000;
constexpr int TEST_SEGMENT_SIZE = 0x200;
extern "C" struct ubcore_target_seg *init_segment(uint64_t va, uint64_t size, struct urma_mem_trans *trans);
TEST_F(TestUbUrma, init_segment)
{
    urma_mem_trans trans = {};
    trans.eid_info.eid_index = TEST_SEGMENT_EID_INDEX;
    MOCKER(ubcore_register_seg).stubs().will(invoke(ubcore_register_seg_stub));
    init_segment(TEST_SEGMENT_VA, TEST_SEGMENT_SIZE, &trans);
    ASSERT_EQ(g_cfg.eid_index, TEST_SEGMENT_EID_INDEX);
    ASSERT_EQ(g_cfg.va, TEST_SEGMENT_VA);
    ASSERT_EQ(g_cfg.len, TEST_SEGMENT_SIZE);
}

constexpr int TEST_SGE_ADDR = 0x1000;
constexpr int TEST_SGE_LEN = 0x200;
extern "C" struct ubcore_sge *init_sge(struct ubcore_target_seg *seg, uint64_t addr, uint64_t len);
TEST_F(TestUbUrma, init_sge)
{
    ubcore_target_seg tsge = {};
    ubcore_sge *sge = init_sge(&tsge, TEST_SGE_ADDR, TEST_SGE_LEN);
    ASSERT_NE(sge, nullptr);
    ASSERT_EQ(sge->tseg, &tsge);
    ASSERT_EQ(sge->addr, TEST_SGE_ADDR);
    ASSERT_EQ(sge->len, TEST_SGE_LEN);
    ::free(sge);
}

extern "C" int init_wr_list(struct ubcore_tjetty *tjetty, struct ubcore_target_seg *lseg,
                            struct ubcore_target_seg *rseg, struct ubcore_jfs_wr **head_wr, uint64_t src_va,
                            uint64_t dst_va, uint32_t source_len);
extern "C" void *kzalloc(size_t size, gfp_t flags);
TEST_F(TestUbUrma, init_wr_list_with_no_mem)
{
    ubcore_target_seg rseg = {};
    ubcore_target_seg lseg = {};
    ubcore_tjetty tjetty = {};
    ubcore_jfs_wr *head_wr = nullptr;
    struct ubcore_jfs_wr *wr = (ubcore_jfs_wr *)malloc(sizeof(ubcore_jfs_wr));
    MOCKER(kzalloc).stubs().will(returnValue((void *)wr)).then(returnValue((void*)nullptr));
    int ret = init_wr_list(&tjetty, &lseg, &rseg, &head_wr, 0, 0, 0);
    ASSERT_EQ(head_wr, nullptr);
    ASSERT_EQ(ret, -ENOMEM);
}
TEST_F(TestUbUrma, init_wr_list)
{
    ubcore_target_seg rseg = {};
    ubcore_target_seg lseg = {};
    ubcore_tjetty tjetty = {};
    ubcore_jfs_wr *head_wr = nullptr;
    ubcore_sge tsge = {};
    MOCKER(init_sge).stubs().will(returnValue(&tsge));
    int ret = init_wr_list(&tjetty, &lseg, &rseg, &head_wr, 0, 0, 0);
    ASSERT_NE(head_wr, nullptr);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(head_wr->opcode, UBCORE_OPC_WRITE);
    ASSERT_EQ(head_wr->rw.dst.num_sge, 1);
    ASSERT_EQ(head_wr->rw.src.num_sge, 1);
}
int init_wr_list_stub(struct ubcore_tjetty *tjetty, struct ubcore_target_seg *lseg, struct ubcore_target_seg *rseg,
                      struct ubcore_jfs_wr **head_wr, uint64_t src_va, uint64_t dst_va, uint32_t source_len)
{
    ubcore_jfs_wr *wr = (ubcore_jfs_wr *)malloc(sizeof(ubcore_jfs_wr));
    if (wr == nullptr) {
        printf("failed malloc\n");
        return 0;
    }
    wr->rw.src.sge = (ubcore_sge *)malloc(sizeof(ubcore_sge));
    wr->rw.dst.sge = (ubcore_sge *)malloc(sizeof(ubcore_sge));
    *head_wr = wr;
    return 0;
}
extern "C" int post_client_jfs(struct ubcore_target_seg *src_seg, struct ubcore_target_seg *dst_seg, uint64_t src_va,
                               uint64_t dst_va, uint32_t source_len);

constexpr int TEST_SEND_SRC = 0x10000;
constexpr int TEST_SEND_DST = 0x20000;
constexpr int TEST_SEND_SIZE = 0x200;
extern "C" int urma_run_send(uint64_t src_va, uint64_t dst_va, struct ubcore_target_seg *src_seg,
                             struct ubcore_target_seg *dst_seg, uint32_t len);
ubcore_jfc_cfg g_jfc_cfg;

extern "C" struct ubcore_jfc *create_jfc(struct urma_mem_trans *trans, ubcore_comp_callback_t jfce_handler);

extern "C" const struct ubcore_jfc_cfg default_jfc_cfg;
TEST_F(TestUbUrma, create_jfc)
{
    MOCKER(ubcore_create_jfc)
        .stubs()
        .will(invoke(+[](struct ubcore_device *dev, struct ubcore_jfc_cfg *cfg, ubcore_comp_callback_t jfce_handler,
                         ubcore_event_callback_t jfae_handler, struct ubcore_udata *udata) -> ubcore_jfc* {
            g_jfc_cfg = *cfg;
            return nullptr;
        }));
    urma_mem_trans trans = {};
    ubcore_comp_callback_t cb = {};
    ubcore_jfc *ret = create_jfc(&trans, cb);
    ASSERT_EQ(ret, nullptr);
    ASSERT_EQ(g_jfc_cfg.ceqn, default_jfc_cfg.ceqn);
    ASSERT_EQ(g_jfc_cfg.depth, default_jfc_cfg.depth);
    ASSERT_EQ(g_jfc_cfg.flag.value, default_jfc_cfg.flag.value);
    ASSERT_EQ(g_jfc_cfg.jfc_context, default_jfc_cfg.jfc_context);
}

constexpr int TEST_JFS_EID_INDEX = 0x100;
ubcore_jfs_cfg g_client_jfs_cfg = {};
extern "C" struct ubcore_jfs *create_client_jfs(struct urma_mem_trans *trans);
TEST_F(TestUbUrma, create_client_jfs)
{
    MOCKER(ubcore_create_jfs)
        .stubs()
        .will(invoke(+[](struct ubcore_device *dev, struct ubcore_jfs_cfg *cfg, ubcore_event_callback_t jfae_handler,
                         struct ubcore_udata *udata) -> ubcore_jfs* {
            g_client_jfs_cfg = *cfg;
            return nullptr;
        }));
    urma_mem_trans trans = {};
    ubcore_jfc jfc = {};
    trans.client_jfc = &jfc;
    trans.eid_info.eid_index = TEST_JFS_EID_INDEX;
    ubcore_jfs *jfs = create_client_jfs(&trans);
    ASSERT_EQ(g_client_jfs_cfg.eid_index, TEST_JFS_EID_INDEX);
    ASSERT_EQ(g_client_jfs_cfg.jfc, &jfc);
}

constexpr int TEST_JFR_EID_INDEX = 0x100;
constexpr int TEST_JFR_EID = 0x1;
constexpr int TEST_JFR_ID = 0x200;
constexpr int TEST_JFR_TOKEN = 0x1000;
extern "C" struct ubcore_tjetty *import_server_jfr(struct urma_mem_trans *trans);
ubcore_tjetty_cfg g_import_server_cfg;
TEST_F(TestUbUrma, import_server_jfr)
{
    MOCKER(ubcore_import_jfr)
        .stubs()
        .will(invoke(+[](struct ubcore_device *dev, struct ubcore_tjetty_cfg *cfg,
                         struct ubcore_udata *udata) -> ubcore_tjetty* {
            g_import_server_cfg = *cfg;
            return nullptr;
        }));
    urma_mem_trans trans = {};
    ubcore_jfr jfr = {};
    jfr.jfr_cfg.eid_index = TEST_JFR_EID_INDEX;
    jfr.jfr_id.id = TEST_JFR_ID;
    jfr.jfr_cfg.trans_mode = UBCORE_TP_RC;
    jfr.jfr_cfg.token_value.token = TEST_JFR_TOKEN;
    jfr.jfr_cfg.flag.bs.token_policy = 0x1;
    trans.eid_info.eid.in4.addr = TEST_JFR_EID;
    trans.server_jfr = &jfr;
    import_server_jfr(&trans);
    ASSERT_EQ(g_import_server_cfg.eid_index, TEST_JFR_EID_INDEX);
    ASSERT_EQ(g_import_server_cfg.id.eid.in4.addr, TEST_JFR_EID);
    ASSERT_EQ(g_import_server_cfg.id.id, TEST_JFR_ID);
    ASSERT_EQ(g_import_server_cfg.trans_mode, UBCORE_TP_RC);
    ASSERT_EQ(g_import_server_cfg.token_value.token, TEST_JFR_TOKEN);
    ASSERT_EQ(g_import_server_cfg.flag.bs.token_policy, 0x1);
}

extern "C" struct list_head dev_list_head;
extern "C" int urma_add_device(struct ubcore_device *dev);
extern "C" void urma_remove_device(struct ubcore_device *dev, void *d __always_unused);
TEST_F(TestUbUrma, add_and_remove_dev)
{
    ASSERT_TRUE(list_empty(&dev_list_head));
    ubcore_device dev;
    urma_add_device(&dev);
    urma_remove_device(&dev, nullptr);
    ASSERT_TRUE(list_empty(&dev_list_head));
}

extern "C" int init_urma_mem_trans(ubcore_comp_callback_t jfce_handler);
extern "C" int get_trans_entity(struct urma_mem_trans *trans);
extern "C" int ubcore_delete_jfc(struct ubcore_jfc *jfc);
extern "C" int ubcore_delete_jfr(struct ubcore_jfr *jfr);
extern "C" int ubcore_delete_jfs(struct ubcore_jfs *jfs);
extern "C" int ubcore_rearm_jfc(struct ubcore_jfc *jfc, bool solicited_only);
extern "C" struct ubcore_jfr *create_server_jfr(struct urma_mem_trans *trans);
struct ubcore_device g_mock_dev = {};
struct ubcore_jfc g_mock_jfc = {};
TEST_F(TestUbUrma, init_urma_mem_trans_part1)
{
    int ret = init_urma_mem_trans(nullptr);
    ASSERT_EQ(ret, -EINVAL);
    GlobalMockObject::verify();
    ubcore_comp_callback_t jfce_handler = +[](struct ubcore_jfc *jfc) {};
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(returnValue(0));
    urma_add_device(&g_mock_dev);
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, -EEXIST);
    GlobalMockObject::verify();
    urma_remove_device(&g_mock_dev, NULL);
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(returnValue(-EFAULT));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, -EFAULT);
    GlobalMockObject::verify();
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(returnValue(0));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, -ENODEV);
    GlobalMockObject::verify();
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(-ENOMEM));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, -ENOMEM);
}

TEST_F(TestUbUrma, init_urma_mem_trans_part2)
{
    ubcore_comp_callback_t jfce_handler = +[](struct ubcore_jfc *jfc) {};
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc)
        .stubs()
        .will(invoke(+[](struct urma_mem_trans *trans, ubcore_comp_callback_t jfce_handler) -> ubcore_jfc* {
            if (jfce_handler == nullptr) {
                return nullptr;
            }
            return &g_mock_jfc;
        }));
    int ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
    GlobalMockObject::verify();
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
        MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc)
        .stubs()
        .will(invoke(+[](struct urma_mem_trans *trans, ubcore_comp_callback_t jfce_handler) -> ubcore_jfc* {
            if (jfce_handler != nullptr) {
                return nullptr;
            }
            return &g_mock_jfc;
        }));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
    GlobalMockObject::verify();
}

TEST_F(TestUbUrma, init_urma_mem_trans_part3)
{
    ubcore_comp_callback_t jfce_handler = +[](struct ubcore_jfc *jfc) {};
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc).stubs().will(returnValue(&g_mock_jfc));
    MOCKER(ubcore_rearm_jfc).stubs().will(returnValue(-EFAULT));
    int ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, -EFAULT);
    GlobalMockObject::verify();
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc).stubs().will(returnValue(&g_mock_jfc));
    MOCKER(create_client_jfs).stubs().will(returnValue(static_cast<ubcore_jfs *>(nullptr)));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
}

TEST_F(TestUbUrma, init_urma_mem_trans_part4)
{
    ubcore_jfs client_jfs = {};
    ubcore_jfr server_jfr = {};
    ubcore_tjetty server_jetty = {};
    ubcore_comp_callback_t jfce_handler = +[](struct ubcore_jfc *jfc) {};
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc).stubs().will(returnValue(&g_mock_jfc));
    MOCKER(create_client_jfs).stubs().will(returnValue(&client_jfs));
    MOCKER(create_server_jfr).stubs().will(returnValue(static_cast<ubcore_jfr *>(nullptr)));
    int ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
    GlobalMockObject::verify();
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc).stubs().will(returnValue(&g_mock_jfc));
    MOCKER(create_client_jfs).stubs().will(returnValue(&client_jfs));
    MOCKER(create_server_jfr).stubs().will(returnValue(&server_jfr));
    MOCKER(import_server_jfr).stubs().will(returnValue(static_cast<ubcore_tjetty *>(nullptr)));
    ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
}
TEST_F(TestUbUrma, init_urma_mem_trans_part5)
{
    ubcore_jfs client_jfs = {};
    ubcore_jfr server_jfr = {};
    ubcore_tjetty server_jetty = {};
    ubcore_comp_callback_t jfce_handler = +[](struct ubcore_jfc *jfc) {};
    MOCKER(ubcore_delete_jfc).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfr).stubs().will(returnValue(0));
    MOCKER(ubcore_delete_jfs).stubs().will(returnValue(0));
    MOCKER(ubcore_register_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_add_device(&g_mock_dev);
        return 0;
    }));
    MOCKER(ubcore_unregister_client).stubs().will(invoke(+[](struct ubcore_client *new_client) -> int {
        urma_remove_device(&g_mock_dev, NULL);
        return 0;
    }));
    MOCKER(get_trans_entity).stubs().will(returnValue(0));
    MOCKER(create_jfc).stubs().will(returnValue(&g_mock_jfc));
    MOCKER(create_client_jfs).stubs().will(returnValue(&client_jfs));
    MOCKER(create_server_jfr).stubs().will(returnValue(&server_jfr));
    MOCKER(import_server_jfr).stubs().will(returnValue(&server_jetty));
    int ret = init_urma_mem_trans(jfce_handler);
    ASSERT_EQ(ret, 0);
}
TEST_F(TestUbUrma, urma_register_segment_test)
{
    ubcore_target_seg rseg = {};
    ubcore_target_seg lseg = {};
    ubcore_tjetty tjetty = {};
    ubcore_jfs_wr *head_wr;
    struct ubcore_sge *sge = (ubcore_sge *)malloc(sizeof(ubcore_sge));
    struct ubcore_jfs_wr *wr = (ubcore_jfs_wr *)malloc(sizeof(ubcore_jfs_wr));
    EXPECT_TRUE(sge != nullptr);
    EXPECT_TRUE(wr != nullptr);
    MOCKER(kzalloc).stubs().will(returnValue((void *)wr));
    MOCKER(init_sge).stubs().will(returnValue(sge)).then(returnValue(static_cast<ubcore_sge *>(nullptr)));
    int ret = init_wr_list(&tjetty, &lseg, &rseg, &head_wr, 0, 0, 0);
    EXPECT_EQ(ret, -ENOMEM);
    GlobalMockObject::verify();
    urma_sge_info sge_info;
    ubcore_target_seg sge2;
    ubcore_target_seg *sge_p = nullptr;
    ubcore_token_id token_id;
    sge2.token_id = &token_id;
    MOCKER(init_segment).stubs().will(returnValue(sge_p));
    ret = urma_register_segment(0, 0, &sge_info);
    EXPECT_EQ(ret, -ENOMEM);
    GlobalMockObject::verify();
    MOCKER(init_segment).stubs().will(returnValue(&sge2));
    MOCKER(ubcore_import_seg).stubs().will(returnValue(sge_p));
    MOCKER(ubcore_unregister_seg).stubs().will(returnValue(0));
    ret = urma_register_segment(0, 0, &sge_info);
    EXPECT_EQ(ret, -ENOMEM);
    GlobalMockObject::verify();
    MOCKER(init_segment).stubs().will(returnValue(&sge2));
    MOCKER(ubcore_import_seg).stubs().will(returnValue(&sge2));
    MOCKER(ubcore_unregister_seg).stubs().will(returnValue(0));
    ret = urma_register_segment(0, 0, &sge_info);
    EXPECT_EQ(ret, 0);
}
extern "C" void free_wr_list(struct ubcore_jfs_wr *wr);
TEST_F(TestUbUrma, urma_free_wr_list_test)
{
    ubcore_jfs_wr *wr = (ubcore_jfs_wr *)malloc(sizeof(ubcore_jfs_wr));
    if (wr == nullptr) {
        printf("failed malloc\n");
    }
    wr->rw.src.sge = (ubcore_sge *)malloc(sizeof(ubcore_sge));
    wr->rw.dst.sge = (ubcore_sge *)malloc(sizeof(ubcore_sge));
    free_wr_list(wr);
    EXPECT_TRUE(wr != nullptr);
}
extern "C" int post_client_jfs(struct ubcore_target_seg *src_seg, struct ubcore_target_seg *dst_seg,
    uint64_t src_va, uint64_t dst_va, uint32_t source_len);
extern "C" void unregister_urma_segment(struct urma_sge_info *sge_info);
TEST_F(TestUbUrma, post_client_jfs_test)
{
    ubcore_target_seg *src_seg = (ubcore_target_seg *)malloc(sizeof(ubcore_target_seg));
    ubcore_target_seg *dst_seg = (ubcore_target_seg *)malloc(sizeof(ubcore_target_seg));
    uint64_t src_va = 0;
    uint64_t dst_va = 0;
    uint32_t source_len = 0;
    MOCKER(init_wr_list).stubs().will(returnValue(1));
    int ret = post_client_jfs(src_seg, dst_seg, src_va, dst_va, source_len);
    EXPECT_EQ(ret, -ENOMEM);
    GlobalMockObject::verify();
    MOCKER(ubcore_post_jfs_wr).stubs().will(returnValue(1));
    MOCKER(init_wr_list).stubs().will(invoke(init_wr_list_stub));
    ret = post_client_jfs(src_seg, dst_seg, src_va, dst_va, source_len);
    EXPECT_EQ(ret, -EFAULT);
}
extern "C" int urma_run_send(uint64_t src_va, uint64_t dst_va, struct ubcore_target_seg *src_seg,
    struct ubcore_target_seg *dst_seg, uint32_t len);
TEST_F(TestUbUrma, urma_run_send_test)
{
    ubcore_target_seg *src_seg = (ubcore_target_seg *)malloc(sizeof(ubcore_target_seg));
    ubcore_target_seg *dst_seg = (ubcore_target_seg *)malloc(sizeof(ubcore_target_seg));
    uint64_t src_va = 0;
    uint64_t dst_va = 0;
    uint32_t len = 0;
    MOCKER(post_client_jfs).stubs().will(returnValue(1));
    int ret = urma_run_send(src_va, dst_va, src_seg, dst_seg, len);
    EXPECT_EQ(ret, 1);
}
extern "C" struct ubcore_jfr *create_server_jfr(struct urma_mem_trans *trans);
TEST_F(TestUbUrma, create_server_jfr_test)
{
    urma_mem_trans *tran = (urma_mem_trans *)malloc(sizeof(urma_mem_trans));
    MOCKER(ubcore_create_jfr).stubs().will(returnValue(static_cast<ubcore_jfr *>(nullptr)));
    ubcore_jfr *ret = create_server_jfr(tran);
    EXPECT_TRUE(ret == nullptr);
}

extern "C" int get_trans_entity(struct urma_mem_trans *trans);
TEST_F(TestUbUrma, get_trans_entity_test)
{
    urma_mem_trans *tran = (urma_mem_trans *)malloc(sizeof(urma_mem_trans));
    MOCKER(ubcore_create_jfr).stubs().will(returnValue(static_cast<ubcore_jfr *>(nullptr)));
    int ret = get_trans_entity(tran);
    EXPECT_EQ(ret, -ENODEV);
}