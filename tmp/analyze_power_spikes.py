"""Characterize short power excursions; attribution remains a hardware hypothesis."""
import json
from collections import Counter

import numpy as np

from analyze_bp_power import SOURCES, OUTPUT, load


def events(t,p,threshold):
    mask = p > threshold
    starts = np.flatnonzero(mask & ~np.r_[False,mask[:-1]])
    ends = np.flatnonzero(mask & ~np.r_[mask[1:],False])
    result = []
    for a,b in zip(starts,ends):
        peak = a + int(np.argmax(p[a:b+1]))
        result.append({'start_s':float(t[a]), 'end_sample_s':float(t[b]),
                       'above_threshold_sample_span_ms':float((b-a+1)*2),
                       'peak_time_s':float(t[peak]),'peak_W':float(p[peak])})
    intervals_ms = np.diff([row['peak_time_s'] for row in result])*1000
    stats = {
        'threshold_W':threshold, 'event_count':len(result),
        'above_threshold_samples':int(mask.sum()), 'sample_fraction_pct':float(mask.mean()*100),
        'median_event_sample_span_ms':float(np.median([r['above_threshold_sample_span_ms'] for r in result])),
        'max_event_sample_span_ms':float(max(r['above_threshold_sample_span_ms'] for r in result)),
        'median_peak_interval_ms':float(np.median(intervals_ms)),
        'most_common_peak_intervals_ms':Counter(np.round(intervals_ms).astype(int).tolist()).most_common(10),
        'phase_concentration_at_102_4_ms':float(abs(np.mean(np.exp(2j*np.pi*np.array([r['peak_time_s'] for r in result])/.1024)))),
        'phase_concentration_at_100_ms':float(abs(np.mean(np.exp(2j*np.pi*np.array([r['peak_time_s'] for r in result])/.1)))),
    }
    return result,stats


def main():
    analyses,datasets = {},{}
    for key,path in SOURCES.items():
        rows,source = load(path)
        rows = [r for r in rows if 0 <= r['时间(ms)'] <= 30000]
        t = np.array([r['时间(ms)']/1000 for r in rows])
        p = np.array([r['功率(W)'] for r in rows])
        v = np.array([r['电压(V)'] for r in rows])
        i = np.array([r['电流(A)'] for r in rows])
        datasets[key] = (t,p)
        mask = p > 1
        stats = {'source':source,'peak_W':float(p.max()),'peak_current_A':float(i.max()),
                 'voltage_median_at_P_above_1W_V':float(np.median(v[mask])),
                 'voltage_median_at_P_below_0_8W_V':float(np.median(v[p<.8])),
                 'current_median_at_P_above_1W_A':float(np.median(i[mask])),
                 'current_median_at_P_below_0_8W_A':float(np.median(i[p<.8])),
                 'current_voltage_correlation':float(np.corrcoef(i,v)[0,1]),
                 'thresholds':{}}
        for threshold in (1,1.5):
            detected,metrics = events(t,p,threshold)
            stats['thresholds'][str(threshold)] = metrics
            (OUTPUT/f'spike_events_{key}_{threshold}W.json').write_text(json.dumps(detected,indent=2),encoding='utf-8')
        centered = mask.astype(float)-mask.mean()
        spectrum = abs(np.fft.rfft(centered*np.hanning(len(centered))))**2
        frequencies = np.fft.rfftfreq(len(centered),d=.002)
        selected = np.flatnonzero((frequencies>=5)&(frequencies<=15))
        dominant = selected[int(np.argmax(spectrum[selected]))]
        stats['dominant_spike_train_frequency_5_to_15_Hz'] = float(frequencies[dominant])
        stats['dominant_spike_train_period_ms'] = float(1000/frequencies[dominant])
        # Inspect autocorrelation in the 80-130 ms interval; event timing is quantized at 2 ms.
        autocorr = [(lag,float(np.dot(centered[:-lag],centered[lag:])/np.dot(centered,centered)))
                    for lag in range(40,66)]
        stats['largest_autocorrelation_80_to_130_ms'] = max(autocorr,key=lambda r:r[1])
        analyses[key] = stats
    (OUTPUT/'spike_summary.json').write_text(json.dumps(analyses,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(analyses,ensure_ascii=False,indent=2))
    plot(datasets)
    report(analyses)


def plot(datasets):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,
                         'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none'})
    fig,axes = plt.subplots(2,2,figsize=(12,7.2),layout='constrained')
    colors = {'off':'#475569','on':'#be123c'}
    labels = {'off':'BP OFF','on':'BP ON'}
    for column,(key,(t,p)) in enumerate(datasets.items()):
        mask=(t>=1)&(t<=2)
        ax=axes[0,column]
        ax.plot(t[mask],p[mask]*1000,color=colors[key],lw=1,marker='.',ms=2)
        ax.axhline(1000,color='#64748b',ls=':',lw=.8)
        ax.set_title(labels[key]+': spikes during 1-2 s',loc='left',fontweight='bold')
        ax.set_ylabel('Power (mW)')
        ax.set_xlabel('Elapsed time (s)')
        ax.set_xlim(1,2)
        ax.set_ylim(550,2050)
        mask=(t>=6)&(t<=9)
        ax=axes[1,column]
        ax.plot(t[mask],p[mask]*1000,color=colors[key],lw=.65)
        ax.set_title(labels[key]+': detail during 6-9 s',loc='left')
        ax.set_ylabel('Power (mW)')
        ax.set_xlabel('Elapsed time (s)')
        ax.set_xlim(6,9)
        ax.set_ylim(600,820)
        ax.text(.01,.96,'Y-axis zoom: taller spikes extend above this panel',transform=ax.transAxes,
                va='top',fontsize=8,color='#475569')
    for ax in axes.flat:
        ax.grid(axis='y',alpha=.18)
    fig.savefig(OUTPUT/'spike_detail.png',dpi=180)
    fig.savefig(OUTPUT/'spike_detail.svg')
    plt.close(fig)


def report(analyses):
    lines = ['# 功率尖峰来源分析', '',
             '**主要推断：两组共有、间隔约 102.4 ms 的窄尖峰最可能来自 Wi-Fi SoftAP 的周期信标发射。** '
             '开启血压计算后出现的较宽、较低的平台状抬升则更可能与计算任务有关。当前证据是功率波形与代码/协议周期的吻合，尚未做同步测量或改变信标周期的因果验证。', '',
             '## 从原始数据观察到的事实', '',
             '统一使用两组各自前 30 秒，以大于 1 W 的连续记录点作为一次“窄高尖峰”事件。1 W 是本次分析阈值，不是器件故障阈值。', '',
             '| 指标 | 关闭血压计算 | 开启血压计算 |', '|---|---:|---:|']
    metrics = [
        ('采样最大功率（W）', lambda a:a['peak_W'],'.5f'),
        ('采样最大输入电流（mA）', lambda a:a['peak_current_A']*1000,'.1f'),
        ('大于 1 W 的连续事件数',lambda a:a['thresholds']['1']['event_count'],'d'),
        ('大于 1 W 的样本点占比（%）',lambda a:a['thresholds']['1']['sample_fraction_pct'],'.3f'),
        ('相邻尖峰时间差中位数（ms）',lambda a:a['thresholds']['1']['median_peak_interval_ms'],'.1f'),
        ('5–15 Hz 候选频段内尖峰序列谱峰（Hz）',lambda a:a['dominant_spike_train_frequency_5_to_15_Hz'],'.3f'),
        ('对应周期（ms）',lambda a:a['dominant_spike_train_period_ms'],'.1f'),
        ('尖峰样本区间中位跨度（ms）',lambda a:a['thresholds']['1']['median_event_sample_span_ms'],'.1f'),
        ('大于 1 W 时输入电压中位数（V）',lambda a:a['voltage_median_at_P_above_1W_V'],'.4f'),
        ('小于 0.8 W 时输入电压中位数（V）',lambda a:a['voltage_median_at_P_below_0_8W_V'],'.4f'),
    ]
    for label,getter,fmt in metrics:
        lines.append(f'| {label} | {format(getter(analyses["off"]),fmt)} | {format(getter(analyses["on"]),fmt)} |')
    lines += ['', '两组共有约 10 次/秒的高尖峰，事件数分别 313 和 314，说明这些尖峰不依赖血压计算开启。'
              '检测到的相邻间隔主要落在 100、102、104 ms，受 2 ms 采样量化、发射时刻抖动及附加事件影响；不能直接用 313/30 当作精确的信标频率。', '',
              '将阈值提高至 1.5 W，规则尖峰仍存在。两组在 102.4 ms 候选周期下的事件相位集中度分别为 '
              f'{analyses["off"]["thresholds"]["1"]["phase_concentration_at_102_4_ms"]:.3f}、'
              f'{analyses["on"]["thresholds"]["1"]["phase_concentration_at_102_4_ms"]:.3f}，'
              '在 100 ms 下分别仅为 '
              f'{analyses["off"]["thresholds"]["1"]["phase_concentration_at_100_ms"]:.3f}、'
              f'{analyses["on"]["thresholds"]["1"]["phase_concentration_at_100_ms"]:.3f}。'
              '该描述量取值 0–1，越接近 1 表示事件越集中于相同周期相位；这里用于支持候选周期，不作为概率或统计置信度。', '',
              '多数高尖峰仅占一个 2 ms 记录点。这个 2 ms 不能解释为已测得真实脉宽，仪器带宽和真实瞬时峰值仍未知。', '',
              '## 为什么首先怀疑 Wi-Fi 信标', '',
              '1. 当前 main 在血压计算开关之外调用 `wifi_init_softap()`，因此关闭模型仍会开启热点。',
              '2. `components/software/WIFI/wifi.c` 的热点配置没有显式设置 `beacon_interval`，使用默认值。',
              '3. 本机 ESP-IDF 6.1 头文件与官方 API 均规定：默认信标间隔为 100 TU，1 TU = 1024 μs，因此周期是 102.4 ms，频率约 9.765625 Hz。这与功率数据候选谱峰约 9.766 Hz 相符。', '',
              '[ESP-IDF 6.1 官方 wifi_ap_config_t 定义](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_wifi.html#_CPPv416wifi_ap_config_t)', '',
              '热点即使没有应用层数据发送，也要周期广播信标。因此“未运行血压模型”不等于“没有无线发射”。'
              '另外，协议任务会把三路采样编码为独立小帧，并在 TCP 客户端连接时提交 Wi-Fi 发送；上位机连接、数据发送及重传可以带来附加的不规则尖峰，但 CSV 无法逐项确认。', '',
              '## 较宽的功率抬升', '',
              '放大 6–9 秒区间，开启计算的数据出现约每秒一次、持续约数百毫秒的较低平台，关闭计算时没有同样清楚的平台。'
              '这与算法每 1 秒产生一个窗口，以及此前另一次测得约 396 ms 的窗口处理耗时相容。'
              '推断它主要对应特征提取、Flash 模型遍历和血压处理的 CPU/存储访问活动。由于不是同步日志，尚不能把每个平台与某次成功计算一一匹配。', '',
              '`sdkconfig` 未开启 CONFIG_PM_ENABLE，因而“动态调频从低频切到高频”不是当前配置下的优先解释；CPU 频率固定也不代表执行负载不会改变电流。', '',
              '## 电压变化与仪器边界', '',
              '高功率点同时有更高电流，输入电压中位数也比低功率点低约 69–84 mV，支持它们与瞬时负载变化相伴。'
              '这不是仅仅将功率列画错了，也不能据此直接断定电源不稳定。供电线阻、输入电容与电源转换电路都会影响仪器端看到的波形；未同步观察板上电源轨和复位信号。', '',
              '## 如何验证推断', '',
              '| 后续对照 | 若信标推断成立，预期看到什么 |', '|---|---|',
              '| 临时将 SoftAP 的 beacon_interval 设为 200 TU，其他条件一致 | 规则高尖峰主周期移到约 204.8 ms，频率约 4.88 Hz |',
              '| 临时关闭 Wi-Fi、保留 USB 数据输出，其他条件尽量一致 | 102.4 ms 的规则高尖峰明显减少或消失 |',
              '| 计算入口/出口翻转 GPIO，并与功率数据同步采集 | 低而宽的平台与计算区间重合 |', '',
              '以上为建议的验证方案，本次没有修改固件配置或执行这些对照。', '',
              '## 对先前平均功耗结果的影响', '',
              '高尖峰是实测输入功率的一部分，原先积分已经保留了它们。不能为了得到更小的模型功耗而删掉尖峰。'
              '关闭和开启状态都存在周期发射负载，等长均值差可估计计算流程增量，但只有一次独立记录/状态，剩余发射波动和仪器误差仍会影响约 2% 的小差值。', '',
              '![尖峰与平台细节](spike_detail.png)', '',
              '`spike_summary.json` 保存统计与原始文件 SHA-256，`spike_events_*.json` 保留阈值事件。原始 CSV 未修改。', '']
    (OUTPUT/'spike_analysis.md').write_text('\n'.join(lines),encoding='utf-8')


if __name__ == '__main__':
    main()
