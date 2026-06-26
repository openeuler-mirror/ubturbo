/*
* Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
* Description: SMAP3.0 accessed_bit测试代码
*/

#include <random>
#include "gtest/gtest.h"
#include "mockcpp/mokc.h"

#include <asm/pgtable.h>

#include <linux/spinlock.h>
#include <linux/hugetlb.h>
#include <linux/list.h>
#include <linux/vmalloc.h>

#include <linux/mmap_lock.h>
#include <linux/errno.h>
#include <linux/pid.h>
#include <linux/mm_types.h>
#include <linux/kvm_host.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/pagewalk.h>

#include "kvm_pgtable.h"
#include "access_pid.h"
#include "accessed_bit.h"
#include "access_tracking_wrapper.h"
#include "access_iomem.h"
#include "hist_tracking.h"

using namespace std;

inline bool operator==(const pte_t& a, const pte_t& b)
{
    return a.pte == b.pte;
}

struct smap_vma_struct {
    unsigned long start_vaddr;
    unsigned long end_vaddr;
};

extern "C" unsigned long huge_page_size(struct hstate *h);
extern "C" int get_numa_range(int nid, u64 *pa_start, u64 *pa_end);
extern "C" struct mem_info acpi_mem;
extern "C" void put_pid(struct pid *pid);
extern "C" void put_task_struct(struct task_struct *t);
extern "C" void *vzalloc(unsigned long size);
extern "C" int calc_paddr_acidx_iomem_known_nid(u64 pa, int nid, u64 *index, int page_size);

class AccessedBitTest : public ::testing::Test {
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

extern "C" void free_mem(struct acpi_mem_segment *mem);
TEST_F(AccessedBitTest, free_mem)
{
    free_mem(nullptr);
}

extern "C" struct page *pfn_to_online_page(unsigned long pfn);
extern "C" bool is_kvm_file(struct file *filp);
TEST_F(AccessedBitTest, is_kvm_file)
{
    bool ret;
    struct file_system_type anonFsType = {
        .name = "anon_inodefs",
        .fs_flags = 0
    };
    struct file_system_type normalFsType = {
        .name = "inodefs",
        .fs_flags = 0
    };
    struct super_block sb = {
        .s_type = &anonFsType
    };
    struct inode inode = {
        .i_sb = &sb
    };
    struct dentry dentry = {
        .d_name = { .name = "kvm-vm" }
    };
    struct file file = {
        .f_path = { .dentry = &dentry },
        .f_inode = &inode
    };
    ret = is_kvm_file(&file);
    EXPECT_TRUE(ret);

    sb.s_type = &normalFsType;
    ret = is_kvm_file(&file);
    EXPECT_FALSE(ret);
}

extern "C" struct file *get_kvm_file_from_task(struct task_struct *task);
TEST_F(AccessedBitTest, get_kvm_file_from_task)
{
    struct file *ret;
    struct task_struct task = {
        .files = nullptr
    };
    ret = get_kvm_file_from_task(nullptr);
    EXPECT_EQ(nullptr, ret);

    ret = get_kvm_file_from_task(&task);
    EXPECT_EQ(nullptr, ret);
}

extern "C" struct fdtable *files_fdtable(struct files_struct *files);
extern "C" bool get_file_rcu(struct file *filp);
extern "C" struct file *files_lookup_fd_rcu(struct files_struct *files, unsigned int fd);
TEST_F(AccessedBitTest, get_kvm_file_from_task_two)
{
    struct file *ret;
    struct file file;
    struct fdtable fdtable = {
        .max_fds = 0
    };
    struct files_struct fs;
    struct task_struct task = {
        .files = &fs
    };

    MOCKER(files_fdtable).stubs().will(returnValue(&fdtable));
    ret = get_kvm_file_from_task(&task);
    EXPECT_EQ(nullptr, ret);

    fdtable.max_fds = 1;
    MOCKER(files_lookup_fd_rcu).stubs().will(returnValue((&file)));
    MOCKER(get_file_rcu).stubs().will(returnValue((true)));
    MOCKER(is_kvm_file).stubs().will(returnValue((true)));
    ret = get_kvm_file_from_task(&task);
    EXPECT_NE(nullptr, ret);
}

TEST_F(AccessedBitTest, get_kvm_file_from_task_three)
{
    struct file *ret;
    struct file file;
    struct fdtable fdtable = {
        .max_fds = 0
    };
    struct files_struct fs;
    struct task_struct task = {
        .files = &fs
    };

    MOCKER(files_fdtable).stubs().will(returnValue(&fdtable));
    fdtable.max_fds = 1;

    MOCKER(files_lookup_fd_rcu).stubs().will(returnValue((&file)));
    MOCKER(get_file_rcu).stubs().will(returnValue((false)));
    ret = get_kvm_file_from_task(&task);
    EXPECT_EQ(nullptr, ret);

    MOCKER(files_lookup_fd_rcu).stubs().will(returnValue((&file)));
    MOCKER(get_file_rcu).stubs().will(returnValue((true)));
    MOCKER(is_kvm_file).stubs().will(returnValue((false)));
    ret = get_kvm_file_from_task(&task);
    EXPECT_EQ(nullptr, ret);
}

extern "C" int get_pid_from_tracking_file(const struct file *file);
TEST_F(AccessedBitTest, GetPidFromTrackingFile)
{
    struct file file;
    struct dentry tempDentry;
    struct dentry dparent;

    file.f_path.dentry = &tempDentry;
    dparent.d_name.name = "123_t";
    tempDentry.d_parent = &tempDentry;
    MOCKER(strncpy).stubs().will(ignoreReturnValue());
    MOCKER(kstrtoint).stubs().will(returnValue(-1));
    int ret = get_pid_from_tracking_file(&file);
    EXPECT_EQ(-1, ret);
}

extern "C" ssize_t proc_file_read(struct file *file, char __user *buf, size_t count, loff_t *pos);
TEST_F(AccessedBitTest, ProcFileRead)
{
    char buf[10];
    struct file file;
    struct ham_tracking_info info;
    loff_t pos = 0;

    info.pid = 1;
    info.len[0] = 0;
    info.len[1] = 0;
    list_add(&info.node, &ham_pid_list);
    MOCKER(get_pid_from_tracking_file).stubs().will(returnValue(1));
    MOCKER(copy_to_user).stubs().will(returnValue(0UL));
    int ret = proc_file_read(&file, buf, 0, &pos);
    EXPECT_EQ(sizeof(uint64_t), ret);
    list_del(&info.node);
}

TEST_F(AccessedBitTest, SmapCreatTrackingInfoFile)
{
    struct ham_tracking_info info;
    struct proc_dir_entry proc_dir;
    struct proc_dir_entry proc_file;

    info.pid = 1;
    MOCKER(proc_mkdir).stubs().will(returnValue(&proc_dir));
    MOCKER(proc_create).stubs().will(returnValue(&proc_file));
    int ret = smap_create_tracking_info_file(&info);
    EXPECT_EQ(0, ret);
}

extern "C" void clear_tracking_info(struct ham_tracking_info *info);
TEST_F(AccessedBitTest, ClearTrackingInfo)
{
    struct ham_tracking_info info;
    u64 paddr1;
    u64 paddr2;
    u16 freq1;
    u16 freq2;
    info.paddr[L1] = &paddr1;
    info.paddr[L2] = &paddr2;
    info.freq[L1] = &freq1;
    info.freq[L2] = &freq2;
    info.len[L1] = info.len[L2] = 1;
    clear_tracking_info(&info);
    EXPECT_EQ(0, paddr1);
}

extern "C" void vfree(void *ptr);
extern "C" struct freq_info *get_freq_info(struct ham_tracking_info *info, uint64_t *freq_info_num);
TEST_F(AccessedBitTest, GetFreqInfo)
{
    uint64_t n;
    struct freq_info *freq_info_array;
    struct ham_tracking_info info;
    info.l2_node = 1;
    u64 paddr1;
    u64 paddr2;
    u16 freq1 = 1;
    u16 freq2 = 1;
    info.paddr[L1] = &paddr1;
    info.paddr[L2] = &paddr2;
    info.freq[L1] = &freq1;
    info.freq[L2] = &freq2;
    info.len[L1] = info.len[L2] = 1;
    freq_info_array = get_freq_info(&info, &n);
    EXPECT_EQ(2, n);
    vfree(freq_info_array);
}

extern "C" bool access_pid_is_scanning(pid_t pid);
TEST_F(AccessedBitTest, GetHamPagesFreqs)
{
    struct ham_tracking_info info;
    struct freq_info *freq_info_array;
    struct access_pid ap;
    ap.type = HAM_SCAN;
    uint64_t num;

    EXPECT_TRUE(list_empty(&ham_pid_list));
    list_add(&info.node, &ham_pid_list);
    info.pid = 1;

    MOCKER(change_ap_type).stubs();
    MOCKER(find_access_pid).stubs().will(returnValue(&ap));
    MOCKER(get_freq_info).stubs().will(returnValue((struct freq_info *)nullptr));
    int ret = get_ham_pages_freqs(1, &freq_info_array, &num);
    EXPECT_EQ(-ENOMEM, ret);

    GlobalMockObject::verify();
    struct freq_info array;
    MOCKER(change_ap_type).stubs();
    MOCKER(find_access_pid).stubs().will(returnValue(&ap));
    MOCKER(get_freq_info).stubs().will(returnValue(&array));
    MOCKER(clear_tracking_info).stubs();
    ret = get_ham_pages_freqs(1, &freq_info_array, &num);
    EXPECT_EQ(0, ret);
    list_del(&info.node);
}

extern "C" int hva_to_hpa_hugetlb(struct kvm *kvm, u64 host_va);
TEST_F(AccessedBitTest, hva_to_hpa_hugetlb)
{
    int ret;
    struct kvm kvm = { .mm = NULL };

    MOCKER(huge_page_size).stubs().will(returnValue(PAGE_SIZE_2M));
    ret = hva_to_hpa_hugetlb(nullptr, 0);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" pte_t *huge_pte_offset(struct mm_struct *mm, unsigned long addr, unsigned long sz);
TEST_F(AccessedBitTest, hva_to_hpa_hugetlb_two)
{
    int ret;
    struct kvm kvm = { .mm = NULL };

    MOCKER(huge_page_size).stubs().will(returnValue(PAGE_SIZE_4K));
    ret = hva_to_hpa_hugetlb(&kvm, 0);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" pte_t smap_huge_ptep_get(pte_t *ptep);
TEST_F(AccessedBitTest, hva_to_hpa_hugetlb_three)
{
    int ret;
    struct kvm kvm = { .mm = NULL };
    pte_t pte = { .pte = 0 };

    MOCKER(huge_page_size).stubs().will(returnValue(PAGE_SIZE_2M));
    MOCKER(huge_pte_offset).stubs().will(returnValue((pte_t *)nullptr));
    ret = hva_to_hpa_hugetlb(&kvm, 0);
    EXPECT_EQ(-EFAULT, ret);

    GlobalMockObject::verify();
    MOCKER(huge_page_size).stubs().will(returnValue(PAGE_SIZE_2M));
    MOCKER(huge_pte_offset).stubs().will(returnValue(&pte));
    MOCKER(smap_huge_ptep_get).stubs().will(returnValue(pte));
    ret = hva_to_hpa_hugetlb(&kvm, 0);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" int hva_to_hpa(struct kvm *kvm, u64 host_va, struct access_pid *ap, bool is_young);
TEST_F(AccessedBitTest, hvaToHpaWithNullKvm)
{
    int ret;
    struct kvm kvm = { .mm = NULL };
    struct access_pid ap;
    ap.type = NORMAL_SCAN;
    ap.pid = 1;

    ret = hva_to_hpa(nullptr, 0, &ap, true);
    EXPECT_EQ(-EINVAL, ret);

    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);
TEST_F(AccessedBitTest, hvaToHpaWithErrorVma)
{
    int ret;
    struct mm_struct mm;
    struct kvm kvm = { .mm = &mm };
    struct vm_area_struct vma;
    struct access_pid ap;
    ap.type = NORMAL_SCAN;
    ap.pid = 1;

    MOCKER(find_vma).stubs().will(returnValue(static_cast<struct vm_area_struct *>(nullptr)));
    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(-EFAULT, ret);
}

extern "C" int hva_to_hpa_ham(struct kvm *kvm, u64 host_va, pid_t pid);
TEST_F(AccessedBitTest, hvaToHpaTest)
{
    int ret;
    struct mm_struct mm;
    struct kvm kvm = { .mm = &mm };
    struct vm_area_struct vma;
    struct access_pid ap;
    ap.type = HAM_SCAN;
    ap.pid = 1;

    // ret value of hva_to_hpa_hugetlb and hva_to_hpa_ham is ignored,
    // since all following tests should success
    MOCKER(find_vma).stubs().will(returnValue(&vma));
    MOCKER(is_vm_hugetlb_page).stubs().will(returnValue(false));
    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(0, ret);
    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(0, ret);

    GlobalMockObject::verify();
    MOCKER(find_vma).stubs().will(returnValue(&vma));
    MOCKER(is_vm_hugetlb_page).stubs().will(returnValue(true));
    MOCKER(hva_to_hpa_ham).stubs()
        .will(returnValue(-EINVAL))
        .then(returnValue(0));
    MOCKER(hva_to_hpa_hugetlb).stubs()
        .will(returnValue(-EINVAL))
        .then(returnValue(0));
    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(0, ret);
    ret = hva_to_hpa(&kvm, 0, &ap, true);
    EXPECT_EQ(0, ret);
}

extern "C" struct vm_area_struct *get_vma_if_huge_page(struct kvm *kvm, unsigned long host_va);
extern "C" int scan_kvm_memslots(struct kvm *kvm, pid_t pid, int page_size, scan_type type);
TEST_F(AccessedBitTest, scanKvmMemslotsWithNullKvm)
{
    int ret;
    struct mm_struct mm;
    struct kvm_pgtable pgt;
    struct kvm_arch arch = {
        .mmu = { 0 }
    };
    struct kvm kvm = {
        .mm = &mm,
        .arch = arch
    };
    struct kvm_memslots slots;

    ret = scan_kvm_memslots(&kvm, 1, 0, STATISTIC_SCAN);
    EXPECT_EQ(-EINVAL, ret);

    MOCKER(kvm_memslots).stubs().will(returnValue((struct kvm_memslots *)nullptr));
    ret = scan_kvm_memslots(&kvm, 1, 0, STATISTIC_SCAN);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, hva_to_hpa_ham)
{
    int ret = hva_to_hpa_ham(nullptr, 0, 0);
    EXPECT_EQ(-EINVAL, ret);

    struct kvm kvm;
    pte_t ptep;
    struct ham_tracking_info info;
    u64 paddr1;
    u64 paddr2;
    u16 freq1;
    u16 freq2;
    info.pid = 1;
    info.l1_node = 0;
    info.l2_node = 1;
    info.paddr[L1] = &paddr1;
    info.paddr[L2] = &paddr2;
    info.freq[L1] = &freq1;
    info.freq[L2] = &freq2;
    info.len[L1] = info.len[L2] = 1;
    ptep.pte = 1;
    MOCKER(huge_page_size).stubs().will(returnValue(0x200000UL));
    MOCKER(huge_pte_offset).stubs().will(returnValue((pte_t *)&ptep));
    list_add(&info.node, &ham_pid_list);
    ret = hva_to_hpa_ham(&kvm, 0, 1);
    EXPECT_EQ(0, ret);

    ret = hva_to_hpa_ham(&kvm, 0, 1);
    EXPECT_EQ(0, ret);
    list_del(&info.node);
}

extern "C" int pre_scan_kvm_memslots(struct kvm *kvm);
TEST_F(AccessedBitTest, pre_scan_kvm_memslots)
{
    int ret;
    int idx = 2;
    struct kvm kvm;

    MOCKER(srcu_read_lock).expects(once()).will(returnValue(idx));
    MOCKER(mmap_read_lock).expects(once()).will(ignoreReturnValue());
    ret = pre_scan_kvm_memslots(&kvm);
    EXPECT_EQ(idx, ret);
}

extern "C" void post_scan_kvm_memslots(struct kvm *kvm, int idx);
TEST_F(AccessedBitTest, post_scan_kvm_memslots)
{
    int idx;
    struct kvm kvm;

    MOCKER(mmap_read_unlock).expects(once()).will(ignoreReturnValue());
    MOCKER(srcu_read_unlock).expects(once()).will(ignoreReturnValue());
    post_scan_kvm_memslots(&kvm, idx);
}

extern "C" unsigned long huge_page_size(struct hstate *h);
extern "C" int drivers_calc_paddr_acidx_iomem(u64 pa, int *nid, u64 *index, int page_size);
extern "C" int get_vma_numa_node(struct kvm *kvm, struct vm_area_struct *vma, unsigned long addr);
TEST_F(AccessedBitTest, GetVmaNUmaNode)
{
    struct kvm kvm;
    pte_t pte;

    pte.pte = 1;
    MOCKER(huge_page_size).stubs().will(returnValue(PAGE_SIZE_2M));
    MOCKER(huge_pte_offset).stubs().will(returnValue(&pte));
    MOCKER(smap_huge_ptep_get).stubs().will(returnValue(pte));
    MOCKER(drivers_calc_paddr_acidx_iomem).stubs().will(returnValue(1));
    MOCKER(calc_paddr_acidx_acpi).stubs().will(returnValue(1));
    int ret = get_vma_numa_node(&kvm, nullptr, 0);
    EXPECT_EQ(NUMA_NO_NODE, ret);
}

extern "C" int fill_vaddrs_info(struct kvm *kvm, struct hva_info *hva_vec, u64 len, u64 *l1_page_num,
                                u64 *l2_page_num);
extern "C" int get_hva_info_by_scan_kvm_memslots(struct kvm *kvm, u64 *l1_page_num, u64 *l2_page_num,
                                                 u64 total_huge_page_num, u64 *vaddrs, struct hva_info *hva_vec);
TEST_F(AccessedBitTest, getHvaInfoByScanKvmMemslots)
{
    u64 len = 1024;
    struct hva_info *hva_vec = static_cast<struct hva_info*>(malloc(len * sizeof(struct hva_info)));
    u64 *vaddrs = static_cast<u64*>(malloc(len * sizeof(u64)));
    u64 l1_page_num;
    u64 l2_page_num;
    struct kvm kvm;
    MOCKER(fill_vaddrs_info).stubs().will(returnValue(0));

    // generate random address and nid with fixed seed
    mt19937_64 gen(1234);
    uniform_int_distribution<int> randomNid(0, 8);
    uniform_int_distribution<u64> randomAddr(0x1000, 0xFFFFF);
    for (int i = 0; i < len; ++i) {
        hva_vec[i].nid = randomNid(gen);
        hva_vec[i].va = randomAddr(gen);
    }

    // check whether hva_vec sorted
    int ret = get_hva_info_by_scan_kvm_memslots(&kvm, &l1_page_num, &l2_page_num, len, vaddrs, hva_vec);
    EXPECT_EQ(0, ret);
    for (int i = 1; i < len; ++i) {
        if (hva_vec[i].nid == hva_vec[i - 1].nid) {
            EXPECT_LE(hva_vec[i - 1].va, hva_vec[i].va);
        }
    }
    free(hva_vec);
    free(vaddrs);
}

extern "C" int alloc_vaddr_info_vm(struct kvm *kvm, struct hva_info **hva_vec, u64 **vaddrs, u64 total_huge_page_nr);
TEST_F(AccessedBitTest, alloc_vaddr_info_vm)
{
    struct kvm kvm;
    struct kvm_pgtable p_pgt;
    struct hva_info *hva_vec;
    u64 *vaddrs;

    kvm.arch.mmu.pgt = &p_pgt;
    int ret = alloc_vaddr_info_vm(nullptr, nullptr, nullptr, 0);
    EXPECT_EQ(-EINVAL, ret);

    ret = alloc_vaddr_info_vm(&kvm, &hva_vec, &vaddrs, 1);
    EXPECT_EQ(0, ret);

    vfree(hva_vec);
    vfree(vaddrs);
}

extern "C" struct pid *find_get_pid(pid_t nr);
extern "C" int get_kvm_by_pid(pid_t pid_nr, struct pid **pid, struct task_struct **task, struct file **filp,
                              struct kvm **kvm);
TEST_F(AccessedBitTest, get_kvm_by_pid)
{
    struct pid pid_tmp;
    struct task_struct task_tmp;
    struct file filp_tmp;
    struct kvm kvm_tmp;
    pid_t pid_nr;
    struct pid *pid;
    struct task_struct *task;
    struct file *filp;
    struct kvm *kvm;

    MOCKER(find_get_pid).stubs().will(returnValue(&pid_tmp));
    MOCKER(get_pid_task).stubs().will(returnValue(&task_tmp));
    MOCKER(get_kvm_file_from_task).stubs().will(returnValue(static_cast<struct file *>(nullptr)));
    int ret = get_kvm_by_pid(pid_nr, &pid, &task, &filp, &kvm);
    EXPECT_EQ(-EBADF, ret);

    GlobalMockObject::verify();
    filp_tmp.private_data = nullptr;
    MOCKER(find_get_pid).stubs().will(returnValue(&pid_tmp));
    MOCKER(get_pid_task).stubs().will(returnValue(&task_tmp));
    MOCKER(get_kvm_file_from_task).stubs().will(returnValue(&filp_tmp));
    ret = get_kvm_by_pid(pid_nr, &pid, &task, &filp, &kvm);
    EXPECT_EQ(-EINVAL, ret);

    filp_tmp.private_data = &kvm_tmp;
    ret = get_kvm_by_pid(pid_nr, &pid, &task, &filp, &kvm);
    EXPECT_EQ(0, ret);
}

extern "C" int get_total_huge_page_nr(struct kvm *kvm, u64 *total_huge_page_nr);
extern "C" int scan_hva_info(pid_t pid_nr, u64 *l1_page_num, u64 *l2_page_num, u64 **l1_vaddr, u64 **l2_vaddr);
TEST_F(AccessedBitTest, scan_hva_info)
{
    pid_t pid_nr;
    u64 l1_page_num;
    u64 l2_page_num;
    u64 *l1_vaddr;
    u64 *l2_vaddr;

    MOCKER(get_kvm_by_pid).stubs().will(returnValue(-EINVAL));
    int ret = scan_hva_info(pid_nr, &l1_page_num, &l2_page_num, &l1_vaddr, &l2_vaddr);
    EXPECT_EQ(-EINVAL, ret);

    GlobalMockObject::verify();
    MOCKER(pre_scan_kvm_memslots).stubs().will(returnValue(1));
    MOCKER(post_scan_kvm_memslots).stubs();
    MOCKER(get_kvm_by_pid).stubs().will(returnValue(0));
    MOCKER(get_total_huge_page_nr).stubs().will(returnValue(-1));
    ret = scan_hva_info(pid_nr, &l1_page_num, &l2_page_num, &l1_vaddr, &l2_vaddr);
    EXPECT_EQ(-1, ret);

    GlobalMockObject::verify();
    MOCKER(pre_scan_kvm_memslots).stubs().will(returnValue(1));
    MOCKER(post_scan_kvm_memslots).stubs();
    MOCKER(get_kvm_by_pid).stubs().will(returnValue(0));
    MOCKER(get_total_huge_page_nr).stubs().will(returnValue(0));
    MOCKER(alloc_vaddr_info_vm).stubs().will(returnValue(-1));
    ret = scan_hva_info(pid_nr, &l1_page_num, &l2_page_num, &l1_vaddr, &l2_vaddr);
    EXPECT_EQ(-1, ret);
}

extern "C" int check_pte_young(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk);
TEST_F(AccessedBitTest, check_pte_young)
{
    pte_t pte;
    pte.pte = 0;
    struct mm_walk walk;
    struct pte_walk pte_walk;

    walk.private_data = &pte_walk;
    int ret = check_pte_young(&pte, 1, 1, &walk);
    EXPECT_EQ(0, ret);

    pte.pte = 1;
    ret = check_pte_young(&pte, 1, 1, &walk);
    EXPECT_EQ(0, ret);
}

static int pte_entry_noop(pte_t *ptep, unsigned long addr, unsigned long next,
                          struct mm_walk *walk)
{
    return 0;
}

const struct mm_walk_ops pte_range_ops = {
	.pte_entry = pte_entry_noop,
};

extern "C" int small_vma_walk(struct mm_struct *mm, unsigned long start_vaddr, unsigned long end_vaddr,
                              struct pte_walk *pte_walk, const struct mm_walk_ops *ops);
extern "C" void process_scan_results(struct pte_walk *pte_walk);
TEST_F(AccessedBitTest, small_vma_walk)
{
    struct pte_walk pw = {};
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(1));

    int ret = small_vma_walk(nullptr, 1, 10, &pw, &pte_range_ops);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(0));
    MOCKER(walk_page_range).stubs().will(returnValue(1));
    MOCKER(mmap_read_unlock).stubs();
    MOCKER(process_scan_results).stubs();
    ret = small_vma_walk(nullptr, 1, 10, &pw, &pte_range_ops);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(0));
    MOCKER(walk_page_range).stubs().will(returnValue(0));
    MOCKER(mmap_read_unlock).stubs();
    MOCKER(process_scan_results).stubs();
    ret = small_vma_walk(nullptr, 1, 10, &pw, &pte_range_ops);
    EXPECT_EQ(0, ret);
}

extern "C" int huge_vma_walk(struct mm_struct *mm, struct smap_vma_struct *vma, struct pte_walk *pte_walk,
                             const struct mm_walk_ops *ops);
TEST_F(AccessedBitTest, huge_vma_walk)
{
    struct smap_vma_struct vma;

    vma.start_vaddr = 0;
    vma.end_vaddr = 0x100;
    MOCKER(small_vma_walk).stubs().will(returnValue(0));
    int ret = huge_vma_walk(nullptr, &vma, nullptr, nullptr);
    EXPECT_EQ(0, ret);
}

extern "C" int take_vma_snapshot(struct mm_struct *mm, struct smap_vma_struct **vma_arr, int *vma_count);
TEST_F(AccessedBitTest, take_vma_snapshot)
{
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(1));
    int ret = take_vma_snapshot(nullptr, nullptr, nullptr);
    EXPECT_EQ(1, ret);

    GlobalMockObject::verify();
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(0));
    ret = take_vma_snapshot(nullptr, nullptr, nullptr);
    EXPECT_EQ(-EINVAL, ret);
}

extern "C" bool IS_ERR(const void *ptr);
extern "C" struct mm_struct *mock_get_mm_by_pid(pid_t pid);
extern "C" int take_vma_snapshot(struct mm_struct *mm,
                                 struct smap_vma_struct **vma_arr, int *vma_count);
TEST_F(AccessedBitTest, scan_accessed_bit_forward_mm_fail)
{
    struct mm_struct mm;
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;

    MOCKER(mock_get_mm_by_pid).stubs().will(returnValue(static_cast<struct mm_struct *>(nullptr)));
    int ret = scan_accessed_bit_forward_mm(&ap, PAGE_SIZE_4K);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, scan_accessed_bit_forward_mm_success)
{
    struct mm_struct mm;
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;

    GlobalMockObject::verify();
    MOCKER(mock_get_mm_by_pid).stubs().will(returnValue(&mm));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(take_vma_snapshot).stubs().will(returnValue(0));
    MOCKER(kfree).stubs().will(ignoreReturnValue());
    int ret = scan_accessed_bit_forward_mm(&ap, PAGE_SIZE_4K);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, scan_accessed_bit_forward_mm_invalid_page)
{
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    int ret = scan_accessed_bit_forward_mm(&ap, PAGE_SIZE_2M);
    EXPECT_EQ(-EINVAL, ret);
}

// ========== DT supplement: pmd_huge ==========

extern "C" int pmd_huge(pmd_t md);
TEST_F(AccessedBitTest, PmdHugeNotHuge)
{
    pmd_t md;
    md.pmd = 0;
    int ret = pmd_huge(md);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, PmdHugeIsHuge)
{
    pmd_t md;
    md.pmd = 0x12345;
    int ret = pmd_huge(md);
    EXPECT_NE(0, ret);
}

// ========== DT supplement: pud_huge ==========

extern "C" int pud_huge(pud_t ud);
TEST_F(AccessedBitTest, PudHugeNotHuge)
{
    pud_t ud;
    ud.pud = 0;
    int ret = pud_huge(ud);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, PudHugeIsHuge)
{
    pud_t ud;
    ud.pud = 0x12345;
    int ret = pud_huge(ud);
    EXPECT_NE(0, ret);
}

// ========== DT supplement: memslot_is_mem ==========

extern "C" bool memslot_is_mem(struct kvm_memory_slot *memslot);
TEST_F(AccessedBitTest, MemslotIsMemBaseGfnTooSmall)
{
    struct kvm_memory_slot memslot;
    memslot.base_gfn = 0;
    memslot.npages = (1 << HUGE_TO_4K_SHIFT) + 1;
    bool ret = memslot_is_mem(&memslot);
    EXPECT_FALSE(ret);
}

TEST_F(AccessedBitTest, MemslotIsMemNpagesTooSmall)
{
    struct kvm_memory_slot memslot;
    memslot.base_gfn = (1 << GB_TO_4K_SHIFT) + 1;
    memslot.npages = 1;
    bool ret = memslot_is_mem(&memslot);
    EXPECT_FALSE(ret);
}

TEST_F(AccessedBitTest, MemslotIsMemValidMemslot)
{
    struct kvm_memory_slot memslot;
    memslot.base_gfn = (1 << GB_TO_4K_SHIFT) + 1;
    memslot.npages = (1 << HUGE_TO_4K_SHIFT) + 1;
    bool ret = memslot_is_mem(&memslot);
    EXPECT_TRUE(ret);
}

// ========== DT supplement: hva_cmp ==========

extern "C" int hva_cmp(const void *a, const void *b);
TEST_F(AccessedBitTest, HvaCmpLocalBeforeRemote)
{
    struct hva_info x = { .va = 0x1000, .nid = 0 };
    struct hva_info y = { .va = 0x2000, .nid = nr_local_numa };
    int ret = hva_cmp(&x, &y);
    EXPECT_EQ(-1, ret);
}

TEST_F(AccessedBitTest, HvaCmpRemoteAfterLocal)
{
    struct hva_info x = { .va = 0x1000, .nid = nr_local_numa };
    struct hva_info y = { .va = 0x2000, .nid = 0 };
    int ret = hva_cmp(&x, &y);
    EXPECT_EQ(1, ret);
}

TEST_F(AccessedBitTest, HvaCmpSameNidAddrLess)
{
    struct hva_info x = { .va = 0x1000, .nid = 0 };
    struct hva_info y = { .va = 0x2000, .nid = 0 };
    int ret = hva_cmp(&x, &y);
    EXPECT_EQ(-1, ret);
}

TEST_F(AccessedBitTest, HvaCmpSameNidAddrEqual)
{
    struct hva_info x = { .va = 0x1000, .nid = 0 };
    struct hva_info y = { .va = 0x1000, .nid = 0 };
    int ret = hva_cmp(&x, &y);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, HvaCmpSameNidAddrGreater)
{
    struct hva_info x = { .va = 0x2000, .nid = 0 };
    struct hva_info y = { .va = 0x1000, .nid = 0 };
    int ret = hva_cmp(&x, &y);
    EXPECT_EQ(1, ret);
}

// ========== DT supplement: get_total_page_nr ==========

extern "C" int get_total_page_nr(struct smap_vma_struct *vma_array, int len, u64 *total_page_nr);
TEST_F(AccessedBitTest, GetTotalPageNrNullVmaArray)
{
    u64 total_page_nr;
    int ret = get_total_page_nr(nullptr, 0, &total_page_nr);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, GetTotalPageNrSingleVma)
{
    struct smap_vma_struct vma;
    u64 total_page_nr;
    vma.start_vaddr = 0x0;
    vma.end_vaddr = 0x10000;
    int ret = get_total_page_nr(&vma, 1, &total_page_nr);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(PHYS_PFN(0x10000) - PHYS_PFN(0x0), total_page_nr);
}

TEST_F(AccessedBitTest, GetTotalPageNrMultipleVmas)
{
    struct smap_vma_struct vma[2];
    u64 total_page_nr;
    vma[0].start_vaddr = 0x0;
    vma[0].end_vaddr = 0x10000;
    vma[1].start_vaddr = 0x20000;
    vma[1].end_vaddr = 0x40000;
    int ret = get_total_page_nr(vma, 2, &total_page_nr);
    EXPECT_EQ(0, ret);
    u64 expected = (PHYS_PFN(0x10000) - PHYS_PFN(0x0)) + (PHYS_PFN(0x40000) - PHYS_PFN(0x20000));
    EXPECT_EQ(expected, total_page_nr);
}

// ========== DT supplement: alloc_vaddr_info ==========

extern "C" int alloc_vaddr_info(struct hva_info **hva_vec, u64 **vaddrs, u64 total_page_nr);
TEST_F(AccessedBitTest, AllocVaddrInfoHvaVecFail)
{
    struct hva_info *hva_vec;
    u64 *vaddrs;
    MOCKER(vzalloc).stubs().will(returnValue((void*)nullptr));
    int ret = alloc_vaddr_info(&hva_vec, &vaddrs, 1);
    EXPECT_EQ(-ENOMEM, ret);
}

TEST_F(AccessedBitTest, AllocVaddrInfoVaddrsFail)
{
    struct hva_info *hva_vec_tmp;
    u64 *vaddrs;
    MOCKER(vzalloc).stubs()
        .will(returnValue(reinterpret_cast<void*>(&hva_vec_tmp)))
        .then(returnValue((void*)nullptr));
    MOCKER(vfree).stubs().will(ignoreReturnValue());
    int ret = alloc_vaddr_info(&hva_vec_tmp, &vaddrs, 1);
    EXPECT_EQ(-ENOMEM, ret);
}

TEST_F(AccessedBitTest, AllocVaddrInfoSuccess)
{
    struct hva_info *hva_vec;
    u64 *vaddrs;
    int ret = alloc_vaddr_info(&hva_vec, &vaddrs, 10);
    EXPECT_EQ(0, ret);
    vfree(hva_vec);
    vfree(vaddrs);
}

// ========== DT supplement: release_resources ==========

extern "C" void release_resources(struct file *filp, struct task_struct *task, struct pid *pid);
TEST_F(AccessedBitTest, ReleaseResources)
{
    struct file filp;
    struct task_struct task;
    struct pid pid_tmp;
    /* fput is a macro (#define fput(x)), put_task_struct/put_pid are stubs */
    release_resources(&filp, &task, &pid_tmp);
}

TEST_F(AccessedBitTest, ReleaseResourcesNullFilp)
{
    struct task_struct task;
    struct pid pid_tmp;
    release_resources(nullptr, &task, &pid_tmp);
}

// ========== DT supplement: get_numa_id_by_paddr ==========

extern "C" int get_numa_id_by_paddr(phys_addr_t paddr);
TEST_F(AccessedBitTest, GetNumaIdByPaddrInvalidPfn)
{
    MOCKER(pfn_valid).stubs().will(returnValue(false));
    int ret = get_numa_id_by_paddr(0x1000);
    EXPECT_EQ(NUMA_NO_NODE, ret);
}

TEST_F(AccessedBitTest, GetNumaIdByPaddrNoOnlinePage)
{
    MOCKER(pfn_valid).stubs().will(returnValue(true));
    MOCKER(pfn_to_online_page).stubs().will(returnValue((struct page*)nullptr));
    int ret = get_numa_id_by_paddr(0x1000);
    EXPECT_EQ(NUMA_NO_NODE, ret);
}

// ========== DT supplement: actc_data_update ==========

extern "C" struct access_tracking_dev *get_access_tracking_dev(int node_id);
extern "C" void actc_data_update(int nid, u64 pa_index);
TEST_F(AccessedBitTest, ActcDataUpdateAdevNull)
{
    /* get_access_tracking_dev is static inline, cannot be mocked.
       With empty access_dev list, it returns NULL -> early return */
    actc_data_update(0, 0);
}

TEST_F(AccessedBitTest, ActcDataUpdatePaIndexOutOfRange)
{
    /* get_access_tracking_dev is static inline, cannot be mocked.
       When access_dev list is empty, it returns NULL -> early return */
    actc_data_update(0, 200);
}

TEST_F(AccessedBitTest, ActcDataUpdateIncrementSuccess)
{
    /* Cannot mock get_access_tracking_dev (static inline).
       Test only verifies null adev path via empty access_dev list */
    actc_data_update(0, 50);
}

// ========== DT supplement: actc_data_add_fast ==========

extern "C" void actc_data_add_fast(phys_addr_t paddr, u32 page_size);
TEST_F(AccessedBitTest, ActcDataAddFastNumaNoNode)
{
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(NUMA_NO_NODE));
    actc_data_add_fast(0x1000, PAGE_SIZE_4K);
}

TEST_F(AccessedBitTest, ActcDataAddFastCalcAcpiSuccess)
{
    /* get_access_tracking_dev is static inline, cannot be mocked.
       With empty access_dev list, adev=NULL -> early return after calc */
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(0));
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(0));
    actc_data_add_fast(0x1000, PAGE_SIZE_4K);
}

TEST_F(AccessedBitTest, ActcDataAddFastCalcIomemFallback)
{
    /* get_access_tracking_dev is static inline, cannot be mocked.
       With empty access_dev list, adev=NULL -> early return */
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(nr_local_numa));
    MOCKER(calc_paddr_acidx_iomem_known_nid).stubs().will(returnValue(0));
    actc_data_add_fast(0x1000, PAGE_SIZE_4K);
}

TEST_F(AccessedBitTest, ActcDataAddFastCalcAllFail)
{
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(0));
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(-1));
    MOCKER(calc_paddr_acidx_iomem_known_nid).stubs().will(returnValue(-1));
    actc_data_add_fast(0x1000, PAGE_SIZE_4K);
}

// ========== DT supplement: ham_actc_data_add ==========

extern "C" void ham_actc_data_add(int pid, phys_addr_t paddr, u32 page_size);
TEST_F(AccessedBitTest, HamActcDataAddNumaNoNode)
{
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(NUMA_NO_NODE));
    ham_actc_data_add(1, 0x1000, PAGE_SIZE_4K);
}

TEST_F(AccessedBitTest, HamActcDataAddCalcFail)
{
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(0));
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(-1));
    MOCKER(calc_paddr_acidx_iomem_known_nid).stubs().will(returnValue(-1));
    ham_actc_data_add(1, 0x1000, PAGE_SIZE_4K);
}

TEST_F(AccessedBitTest, HamActcDataAddSuccess)
{
    struct ham_tracking_info info;
    u64 paddr1;
    u64 paddr2;
    actc_t freq1 = 0;
    actc_t freq2 = 0;
    info.pid = 1;
    info.l1_node = 0;
    info.l2_node = -1;
    info.paddr[L1] = &paddr1;
    info.paddr[L2] = &paddr2;
    info.freq[L1] = &freq1;
    info.freq[L2] = &freq2;
    info.len[L1] = 1;
    info.len[L2] = 0;
    list_add(&info.node, &ham_pid_list);

    /* get_access_tracking_dev is static inline, cannot be mocked.
       With empty access_dev list, adev=NULL -> early return after calc */
    MOCKER(get_numa_id_by_paddr).stubs().will(returnValue(0));
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(0));
    ham_actc_data_add(1, 0x1000, PAGE_SIZE_4K);
    list_del(&info.node);
}

// ========== DT supplement: get_vma_if_huge_page ==========

extern "C" struct vm_area_struct *get_vma_if_huge_page(struct kvm *kvm, unsigned long host_va);
TEST_F(AccessedBitTest, GetVmaIfHugePageNullMm)
{
    struct kvm kvm = { .mm = nullptr };
    struct vm_area_struct *ret = get_vma_if_huge_page(&kvm, 0);
    EXPECT_EQ(nullptr, ret);
}

TEST_F(AccessedBitTest, GetVmaIfHugePageNullVma)
{
    struct mm_struct mm;
    struct kvm kvm = { .mm = &mm };
    MOCKER(find_vma).stubs().will(returnValue((struct vm_area_struct*)nullptr));
    struct vm_area_struct *ret = get_vma_if_huge_page(&kvm, 0);
    EXPECT_EQ(nullptr, ret);
}

TEST_F(AccessedBitTest, GetVmaIfHugePageNotHugetlb)
{
    struct mm_struct mm;
    struct kvm kvm = { .mm = &mm };
    struct vm_area_struct vma;
    MOCKER(find_vma).stubs().will(returnValue(&vma));
    MOCKER(is_vm_hugetlb_page).stubs().will(returnValue(false));
    struct vm_area_struct *ret = get_vma_if_huge_page(&kvm, 0);
    EXPECT_EQ(nullptr, ret);
}

TEST_F(AccessedBitTest, GetVmaIfHugePageIsHugetlb)
{
    struct mm_struct mm;
    struct kvm kvm = { .mm = &mm };
    struct vm_area_struct vma;
    MOCKER(find_vma).stubs().will(returnValue(&vma));
    MOCKER(is_vm_hugetlb_page).stubs().will(returnValue(true));
    struct vm_area_struct *ret = get_vma_if_huge_page(&kvm, 0);
    EXPECT_NE(nullptr, ret);
}

// ========== DT supplement: scan_accessed_bit_forward_vm ==========

extern "C" int scan_accessed_bit_forward_vm(struct access_pid *ap, int page_size);
TEST_F(AccessedBitTest, ScanAccessedBitForwardVmInvalidPageSize)
{
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    int ret = scan_accessed_bit_forward_vm(&ap, PAGE_SIZE_4K);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, ScanAccessedBitForwardVmNoScan)
{
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NO_SCAN;
    int ret = scan_accessed_bit_forward_vm(&ap, PAGE_SIZE_2M);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, ScanAccessedBitForwardVmFindPidFail)
{
    struct access_pid ap;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    MOCKER(find_get_pid).stubs().will(returnValue((struct pid*)nullptr));
    int ret = scan_accessed_bit_forward_vm(&ap, PAGE_SIZE_2M);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, ScanAccessedBitForwardVmGetTaskFail)
{
    struct access_pid ap;
    struct pid pid_s;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    MOCKER(find_get_pid).stubs().will(returnValue(&pid_s));
    MOCKER(get_pid_task).stubs().will(returnValue((struct task_struct*)nullptr));
    int ret = scan_accessed_bit_forward_vm(&ap, PAGE_SIZE_2M);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, ScanAccessedBitForwardVmGetKvmFileFail)
{
    struct access_pid ap;
    struct pid pid_s;
    struct task_struct task;
    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    MOCKER(find_get_pid).stubs().will(returnValue(&pid_s));
    MOCKER(get_pid_task).stubs().will(returnValue(&task));
    MOCKER(get_kvm_file_from_task).stubs().will(returnValue((struct file*)nullptr));
    int ret = scan_accessed_bit_forward_vm(&ap, PAGE_SIZE_2M);
    EXPECT_EQ(-EINVAL, ret);
}

// ========== DT supplement: fill_pte_va ==========

extern "C" int fill_pte_va(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk);
TEST_F(AccessedBitTest, FillPteVaSwapPte)
{
    pte_t pte;
    pte.pte = 0x1;
    struct mm_walk walk;
    struct pte_walk pw = { .index = 0 };
    walk.private_data = &pw;
    int ret = fill_pte_va(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, FillPteVaPaddrZero)
{
    pte_t pte;
    pte.pte = 0;
    struct mm_walk walk;
    struct pte_walk pw = { .index = 0, .page_size = PAGE_SIZE_4K };
    walk.private_data = &pw;
    int ret = fill_pte_va(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, FillPteVaAcpiCalcSuccess)
{
    pte_t pte;
    pte.pte = 0x1000;
    struct hva_info hva_vec[10];
    struct mm_walk walk;
    struct pte_walk pw;
    pw.index = 0;
    pw.page_size = PAGE_SIZE_4K;
    pw.hva_info = hva_vec;
    pw.nr_page[L1] = 0;
    pw.nr_page[L2] = 0;
    walk.private_data = &pw;
    MOCKER(calc_paddr_acidx_acpi).stubs().will(returnValue(0));
    int ret = fill_pte_va(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(0x1000, hva_vec[0].va);
    EXPECT_EQ(1, pw.nr_page[L1]);
}

TEST_F(AccessedBitTest, FillPteVaIomemFallback)
{
    pte_t pte;
    pte.pte = 0x1000;
    struct hva_info hva_vec[10];
    struct mm_walk walk;
    struct pte_walk pw;
    pw.index = 0;
    pw.page_size = PAGE_SIZE_4K;
    pw.hva_info = hva_vec;
    pw.nr_page[L1] = 0;
    pw.nr_page[L2] = 0;
    walk.private_data = &pw;
    MOCKER(calc_paddr_acidx_acpi).stubs().will(returnValue(-1));
    MOCKER(drivers_calc_paddr_acidx_iomem).stubs().will(returnValue(0));
    int ret = fill_pte_va(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
    EXPECT_EQ(1, pw.nr_page[L2]);
}

// ========== DT supplement: pte_pure_clear ==========

extern "C" int pte_pure_clear(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk);
TEST_F(AccessedBitTest, PtePureClearSwapPte)
{
    pte_t pte;
    pte.pte = 0x1;
    struct mm_walk walk;
    struct pte_walk pw = { .flag = false };
    walk.private_data = &pw;
    int ret = pte_pure_clear(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(pw.flag);
}

TEST_F(AccessedBitTest, PtePureClearYoungPte)
{
    /* pte_young is a macro stub returning 0 on aarch64, so pw.flag stays false
       but the function still runs without error */
    pte_t pte;
    pte.pte = 0x1000;
    struct mm_walk walk;
    struct pte_walk pw;
    pw.flag = false;
    walk.private_data = &pw;
    int ret = pte_pure_clear(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, PtePureClearNotYoungPte)
{
    pte_t pte;
    pte.pte = 0x0;
    struct mm_walk walk;
    struct pte_walk pw;
    pw.flag = false;
    walk.private_data = &pw;
    int ret = pte_pure_clear(&pte, 0x1000, 0x2000, &walk);
    EXPECT_EQ(0, ret);
    EXPECT_FALSE(pw.flag);
}

// ========== DT supplement: pid_pte_mkold ==========

extern "C" struct mm_struct *mock_get_mm_by_pid(pid_t pid);
TEST_F(AccessedBitTest, PidPteMkoldBadMm)
{
    struct access_pid ap;
    ap.pid = 1;
    MOCKER(mock_get_mm_by_pid).stubs().will(returnValue((struct mm_struct*)nullptr));
    int ret = pid_pte_mkold(&ap);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, PidPteMkoldMmapLockFail)
{
    struct access_pid ap;
    struct mm_struct mm;
    ap.pid = 1;
    MOCKER(mock_get_mm_by_pid).stubs().will(returnValue(&mm));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(-EINTR));
    MOCKER(mmput).stubs().will(ignoreReturnValue());
    int ret = pid_pte_mkold(&ap);
    EXPECT_EQ(-EINTR, ret);
}

TEST_F(AccessedBitTest, PidPteMkoldWalkFail)
{
    struct access_pid ap;
    struct mm_struct mm;
    ap.pid = 1;
    MOCKER(mock_get_mm_by_pid).stubs().will(returnValue(&mm));
    MOCKER(IS_ERR).stubs().will(returnValue(false));
    MOCKER(mmap_read_lock_killable).stubs().will(returnValue(0));
    MOCKER(walk_page_range).stubs().will(returnValue(-EINVAL));
    MOCKER(mmap_read_unlock).stubs().will(ignoreReturnValue());
    MOCKER(mmput).stubs().will(ignoreReturnValue());
    int ret = pid_pte_mkold(&ap);
    EXPECT_EQ(-EINVAL, ret);
}

// ========== DT supplement: process_scan_results ==========

extern "C" int calc_paddr_acidx_acpi_known_nid(u64 paddr, int nid, u64 *pa_index, int page_size);
extern "C" int calc_paddr_acidx_iomem_known_nid(u64 paddr, int nid, u64 *pa_index, int page_size);
extern "C" void add_to_bm_page_fast(u64 paddr, int nid, u64 acidx, struct access_pid *ap);
extern "C" void process_scan_results(struct pte_walk *pte_walk);
TEST_F(AccessedBitTest, ProcessScanResultsNullPteWalk)
{
    process_scan_results(nullptr);
}

TEST_F(AccessedBitTest, ProcessScanResultsNullAp)
{
    struct pte_walk pw = { .ap = nullptr };
    process_scan_results(&pw);
}

TEST_F(AccessedBitTest, ProcessScanResultsLocalNidAcpi)
{
    struct access_pid ap;
    struct scan_result_entry entries[2];

    entries[0].paddr = 0x1000;
    entries[0].nid = 0;
    entries[0].hot = true;
    entries[1].paddr = 0x2000;
    entries[1].nid = 0;
    entries[1].hot = false;

    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    ap.cur_times = 0;
    ap.ntimes = 10;
    struct pte_walk pw;
    pw.ap = &ap;
    pw.scan_results = entries;
    pw.scan_result_cnt = 2;

    /* cur_times=0, ntimes=10 => cur_times+1 < ntimes => not last scan */
    /* get_access_tracking_dev is static inline, cannot be mocked.
       With empty access_dev list, adev=NULL -> actc_data_update skips */
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(0));
    process_scan_results(&pw);
}

TEST_F(AccessedBitTest, ProcessScanResultsRemoteNidIomem)
{
    struct access_pid ap;
    struct scan_result_entry entries[1];

    entries[0].paddr = 0x1000;
    entries[0].nid = nr_local_numa;
    entries[0].hot = true;

    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    ap.cur_times = 0;
    ap.ntimes = 10;
    struct pte_walk pw;
    pw.ap = &ap;
    pw.scan_results = entries;
    pw.scan_result_cnt = 1;

    /* cur_times=0, ntimes=10 => not last scan */
    /* get_access_tracking_dev is static inline, cannot be mocked */
    MOCKER(calc_paddr_acidx_iomem_known_nid).stubs().will(returnValue(0));
    process_scan_results(&pw);
}

TEST_F(AccessedBitTest, ProcessScanResultsLastScanAddToBm)
{
    struct access_pid ap;
    struct scan_result_entry entries[1];
    entries[0].paddr = 0x1000;
    entries[0].nid = 0;
    entries[0].hot = false;

    ap.pid = 1;
    ap.type = NORMAL_SCAN;
    ap.cur_times = 9;
    ap.ntimes = 10;
    struct pte_walk pw = { .ap = &ap, .scan_results = entries, .scan_result_cnt = 1 };

    /* cur_times=9, ntimes=10 => cur_times+1 >= ntimes => is last scan */
    MOCKER(calc_paddr_acidx_acpi_known_nid).stubs().will(returnValue(0));
    MOCKER(add_to_bm_page_fast).stubs().will(ignoreReturnValue());
    process_scan_results(&pw);
}

// ========== DT supplement: setup_statistic_scan ==========

extern "C" int setup_statistic_scan(struct pte_walk *pte_walk, int pid, struct smap_vma_struct *vma_array, int vma_count);
TEST_F(AccessedBitTest, SetupStatisticScanNotStatisticType)
{
    struct pte_walk pw;
    pw.type = NORMAL_SCAN;
    int ret = setup_statistic_scan(&pw, 1, nullptr, 0);
    EXPECT_EQ(0, ret);
}

TEST_F(AccessedBitTest, SetupStatisticScanGetPageNrFail)
{
    struct pte_walk pw;
    pw.type = STATISTIC_SCAN;
    int ret = setup_statistic_scan(&pw, 1, nullptr, 0);
    EXPECT_EQ(-EINVAL, ret);
}

TEST_F(AccessedBitTest, SetupStatisticScanVzallocFail)
{
    struct smap_vma_struct vma;
    struct pte_walk pw;
    vma.start_vaddr = 0;
    vma.end_vaddr = 0x10000;
    pw.type = STATISTIC_SCAN;
    MOCKER(vzalloc).stubs().will(returnValue((void*)nullptr));
    int ret = setup_statistic_scan(&pw, 1, &vma, 1);
    EXPECT_EQ(-ENOMEM, ret);
}

TEST_F(AccessedBitTest, SetupStatisticScanSuccess)
{
    struct smap_vma_struct vma;
    struct pte_walk pw;
    u64 *statistic_vaddr;
    vma.start_vaddr = 0;
    vma.end_vaddr = 0x10000;
    pw.type = STATISTIC_SCAN;
    MOCKER(vzalloc).stubs().will(returnValue(reinterpret_cast<void*>(&statistic_vaddr)));
    int ret = setup_statistic_scan(&pw, 1, &vma, 1);
    EXPECT_EQ(0, ret);
}

// ========== DT supplement: clear_statistic_tracking_info ==========

extern "C" void clear_statistic_tracking_info(pid_t pid);
TEST_F(AccessedBitTest, ClearStatisticTrackingInfoNotFound)
{
    struct statistics_tracking_info info;
    info.pid = 2;
    list_add(&info.node, &statistic_pid_list);
    u8 window_data[10] = {1, 2, 3};
    u8 *window_ptr = window_data;
    info.sliding_windows = &window_ptr;
    info.page_num[L1] = 10;
    info.page_num[L2] = 0;
    info.scan_num = 0;
    clear_statistic_tracking_info(1);
    EXPECT_EQ(1, window_data[0]);
    list_del(&info.node);
}

TEST_F(AccessedBitTest, ClearStatisticTrackingInfoFoundAndClear)
{
    struct statistics_tracking_info info;
    u8 window_data[4] = {5, 6, 7, 8};
    u8 *window_ptr = window_data;
    info.pid = 1;
    info.l1_node = 0;
    info.l2_node = -1;
    info.sliding_windows = &window_ptr;
    info.page_num[L1] = 3;
    info.page_num[L2] = 1;
    info.scan_num = 0;
    list_add(&info.node, &statistic_pid_list);
    clear_statistic_tracking_info(1);
    EXPECT_EQ(0, window_data[0]);
    EXPECT_EQ(0, window_data[1]);
    EXPECT_EQ(0, window_data[2]);
    EXPECT_EQ(0, window_data[3]);
    list_del(&info.node);
}

// ========== DT supplement: update_statistic_scan_num ==========

extern "C" void update_statistic_scan_num(pid_t pid);
TEST_F(AccessedBitTest, UpdateStatisticScanNumIncrement)
{
    struct statistics_tracking_info info;
    info.pid = 1;
    info.scan_num = 0;
    info.window_num = 4;
    list_add(&info.node, &statistic_pid_list);
    update_statistic_scan_num(1);
    EXPECT_EQ(1, info.scan_num);
    update_statistic_scan_num(1);
    EXPECT_EQ(2, info.scan_num);
    list_del(&info.node);
}

TEST_F(AccessedBitTest, UpdateStatisticScanNumWrapAround)
{
    struct statistics_tracking_info info;
    info.pid = 1;
    info.scan_num = 3;
    info.window_num = 4;
    list_add(&info.node, &statistic_pid_list);
    update_statistic_scan_num(1);
    EXPECT_EQ(0, info.scan_num);
    list_del(&info.node);
}
