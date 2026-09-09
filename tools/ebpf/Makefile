NR_HUGEPAGES ?= 20
HUGETLBFS_MNT ?= /mnt/huge

.PHONY: setup setup-hugetlbfs

setup:
	sudo sh -c 'echo $(NR_HUGEPAGES) > /proc/sys/vm/nr_hugepages'

setup-hugetlbfs: setup
	sudo mkdir -p $(HUGETLBFS_MNT)
	sudo mount -t hugetlbfs none $(HUGETLBFS_MNT)
