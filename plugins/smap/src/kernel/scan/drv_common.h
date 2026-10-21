/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Description: SMAP: tracking_common
 */

#ifndef __TRACKING_COMMON_H__
#define __TRACKING_COMMON_H__

/*
 * actc_t 为频次的"导出/传输"类型：经 sqrt 压缩后落在 u8/256 桶范围内，
 * 与用户态桶排序 FREQ_BUCKETS_SIZE(256) 对齐。内部累加数组请使用 u16
 * 独立类型以避免在 256 处回绕丢失真值。
 */
typedef u8 actc_t;

/*
 * compress_freq - 将 raw 频次开根号压缩进 actc_t(u8)。
 * @raw: 内核累加的原始频次（0..65535）
 *
 * 逐位试凑 floor(sqrt(raw))：从高位到低位试置 bit，若 (试凑值)^2 <= raw 则保留。
 * raw <= 65535 时结果 <= 255（255^2=65025<=65535<256^2），天然落在 u8/256 桶内，
 * 单调无断崖。(r|bit) <= 0xffff，平方不超过 0xfffe0001，在 u32 内不溢出。
 * 自包含实现，不依赖内核 int_sqrt，便于用户态单测与 UT 桩环境链接。
 */
static inline actc_t compress_freq(u16 raw)
{
	u32 r = 0;
	u32 bit;

	for (bit = 0x8000; bit != 0; bit >>= 1) {
		u32 t = (r | bit) * (r | bit);
		if (t <= (u32)raw)
			r |= bit;
	}
	return (actc_t)r;
}

#endif /* __TRACKING_COMMON_H__ */
