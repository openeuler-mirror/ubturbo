/*
 * Description: SMAP3.0 虚机内存地址模块桩文件
 */

unsigned long _compound_head(struct page *page)
{
	return (unsigned long)page;
}
