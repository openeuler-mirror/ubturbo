/*
 * Description: SMAP 测试代码
 */

#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <linux/fs.h>
#include <linux/kmod.h>
#include <linux/slab.h>
#include <linux/umh.h>
#include "dump_info.h"

extern struct list_head pid_list;

class DumpInfoTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        std::cout << "[Phase SetUp Begin]" << std::endl;
        std::cout << "[Phase SetUp End]" << std::endl;
    }
    void TearDown() override
    {
        std::cout << "[Phase TearDown Begin]" << std::endl;
        GlobalMockObject::reset();
        std::cout << "[Phase TearDown End]" << std::endl;
    }
};

extern "C" void filter_4k_migrate_info(size_t *nr_abnormal);
TEST_F(DumpInfoTest, Filter4kMigrateInfo)
{
    size_t nr_abnormal[NR_ABNORMAL] = {0};
    filter_4k_migrate_info(nr_abnormal);
    GlobalMockObject::verify();
}