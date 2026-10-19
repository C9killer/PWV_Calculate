"""Analyze no-Wi-Fi power recordings; preserve all raw observations."""
import json
import statistics
from pathlib import Path

import numpy as np

from analyze_bp_power import ROOT, load, measure, difference

OUTPUT = ROOT / 'reports' / 'bp_power_no_wifi_20261019'
SOURCES = {
    'off': Path('C:/Users/T/Desktop/未加载血压计算数据（无wifi）.csv'),
    'on': Path('C:/Users/T/Desktop/运行血压数据(无wifi).csv'),
}


def excursions(rows, start=0, end=30):
    """Descriptive 20 ms power envelope, not synchronized task timing."""
    chosen = [r for r in rows if start <= r['时间(ms)']/1000 <= end]
    t = np.array([r['时间(ms)']/1000 for r in chosen])
    p = np.array([r['功率(W)'] for r in chosen])
    # Ten 2 ms samples. Center timestamps are half-sample shifted.
    smooth = np.convolve(p, np.ones(10)/10, mode='valid')
    st = np.convolve(t, np.ones(10)/10, mode='valid')
    mask = smooth > .55
    starts = np.flatnonzero(mask & ~np.r_[False, mask[:-1]])
    ends = np.flatnonzero(mask & ~np.r_[mask[1:], False])
    merged = []
    for a, b in zip(starts, ends):
        if merged and st[a]-st[merged[-1][1]] <= .05:
            merged[-1][1] = int(b)
        else:
            merged.append([int(a), int(b)])
    result = []
    for a, b in merged:
        duration = st[b]-st[a]+.002
        if duration < .1:
            continue
        lo, hi = float(st[a]-.001), float(st[b]+.001)
        metrics = measure(rows, lo, hi)
        result.append({'start_s':lo, 'end_s':hi, 'duration_ms':float(duration*1000),
                       'mean_power_W':metrics['mean_power_W'],
                       'max_power_W':metrics['max_power_W']})
    durations = [r['duration_ms'] for r in result]
    intervals = [1000*(b['start_s']-a['start_s']) for a,b in zip(result,result[1:])]
    return {
        'method':'20 ms moving-average power > 0.55 W; merge gaps <= 50 ms; keep spans >= 100 ms',
        'threshold_W':.55, 'events':result, 'count':len(result),
        'median_span_ms': statistics.median(durations) if durations else None,
        'min_span_ms': min(durations) if durations else None,
        'max_span_ms': max(durations) if durations else None,
        'median_start_interval_ms': statistics.median(intervals) if intervals else None,
        'median_event_mean_power_W':statistics.median(r['mean_power_W'] for r in result) if result else None,
        'above_1W_sample_count':int(np.count_nonzero(p>1)),
        'above_1W_event_count':int(np.count_nonzero((p>1) & ~np.r_[False,(p>1)[:-1]])),
    }


def compare(datasets, a, b):
    conditions = {key:measure(rows,a,b) for key,rows in datasets.items()}
    return {'conditions':conditions,'difference':difference(conditions['off'],conditions['on'])}


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    datasets, sources = {}, {}
    for key,path in SOURCES.items():
        datasets[key], sources[key] = load(path)
        assert sources[key]['dt_ms_counts'] == {2.0: sources[key]['rows']-1}
    common_end = min(r['end_s'] for r in sources.values())
    summary = {
        'condition_labels':'Both Wi-Fi disabled; BP calculation off/on according to supplied filenames',
        'comparison_assumption':'Same supply, acquisition, signal, serial output and instrument settings except BP switch; not independently verified from CSV',
        'sources':sources,
        'primary_0_to_30_s':compare(datasets,0,30),
        'sensitivity_5_to_30_s':compare(datasets,5,30),
        'all_common_0_to_end':compare(datasets,0,common_end),
        'all_common_5_to_end':compare(datasets,5,common_end),
        'full_records':{key:measure(rows,sources[key]['start_s'],sources[key]['end_s']) for key,rows in datasets.items()},
        'five_second_blocks':[{'start_s':a,'end_s':a+5,**compare(datasets,a,a+5)} for a in range(0,30,5)],
        'wide_excursions_0_to_30_s':{key:excursions(rows) for key,rows in datasets.items()},
        'methods':{
            'mean_power':'Trapezoidal integral of raw recorded power divided by elapsed time; no peaks removed',
            'delta':'Mean BP-on minus mean BP-off in independent equal-duration recordings; not synchronized subtraction',
            'primary':'0-30 s for consistency with earlier Wi-Fi-enabled report',
            'sensitivity':'5-30 s excludes first five seconds but does not establish startup time or steady state',
            'counter_check':'Compare raw-power integral with cumulative Wh(end)-Wh(start), times 3600',
        },
        'limitations':[
            'One independent recording per condition; samples/5 s blocks are not repeated trials.',
            'No synchronized task markers or output count; no measured per-inference energy or runtime.',
            'Input power at instrument boundary includes acquisition, peripheral loads, serial output and conversion losses.',
            'Increment includes BP feature extraction, model inference, postprocessing and their downstream overhead.',
            'Power-envelope widths are descriptive and depend on threshold and smoothing; not task runtimes.',
            '2 ms export interval does not establish instrument bandwidth, instantaneous peak or accuracy.',
        ],
    }
    old = json.loads((ROOT/'reports/bp_power_20261019/summary.json').read_text(encoding='utf-8'))
    old_spikes = json.loads((ROOT/'reports/bp_power_20261019/spike_summary.json').read_text(encoding='utf-8'))
    summary['earlier_wifi_enabled'] = {
        'source_report':'../bp_power_20261019/report.md',
        'primary':old['primary'],'difference':old['difference'],
        'sensitivity_5_to_30_s':old['sensitivity_5_to_30s'],
        'spikes':old_spikes,
        'off_mean_power_reduction_mW':1000*(old['primary']['off']['mean_power_W']-summary['primary_0_to_30_s']['conditions']['off']['mean_power_W']),
        'on_mean_power_reduction_mW':1000*(old['primary']['on']['mean_power_W']-summary['primary_0_to_30_s']['conditions']['on']['mean_power_W']),
    }
    (OUTPUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    plot(datasets, summary)
    write_report(summary)
    print(json.dumps({
        'report':str(OUTPUT/'report.md'),
        'primary':summary['primary_0_to_30_s'],
        'sensitivity':summary['sensitivity_5_to_30_s'],
        'common':summary['all_common_0_to_end']['difference'],
        'blocks':[(b['start_s'],b['difference']['incremental_mean_power_mW']) for b in summary['five_second_blocks']],
        'wide':{k:{f:v for f,v in r.items() if f!='events'} for k,r in summary['wide_excursions_0_to_30_s'].items()},
    },ensure_ascii=False,indent=2))


def plot(data, summary):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,
                         'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none'})
    colors = {'off':'#475569','on':'#be123c'}
    fig,axes = plt.subplots(3,1,figsize=(11,9),layout='constrained',gridspec_kw={'height_ratios':[1,1.2,1]})
    for key,rows in data.items():
        chosen=[r for r in rows if r['时间(ms)']<=30000]
        t=np.array([r['时间(ms)']/1000 for r in chosen])
        p=np.array([r['功率(W)']*1000 for r in chosen])
        axes[0].plot(t,p,color=colors[key],lw=.5,alpha=.75,label='BP '+key.upper())
        detail=(t>=4.8)&(t<=9)
        axes[1].plot(t[detail],p[detail],color=colors[key],lw=.6,alpha=.7,label='BP '+key.upper())
    for ax in axes[:2]:
        ax.set_ylabel('Power (mW)')
        ax.set_xlabel('Elapsed time from each recording start (s)')
        ax.legend(loc='upper right',frameon=False,ncols=2)
        ax.grid(axis='y',alpha=.18)
        ax.set_ylim(480,650)
    axes[0].set_xlim(0,30)
    axes[0].set_title('Wi-Fi OFF: raw power, first 30 s (independent runs)',loc='left',fontweight='bold')
    axes[1].set_xlim(4.8,9)
    axes[1].set_title('Wide power elevations after enabling BP calculation',loc='left')
    for event in summary['wide_excursions_0_to_30_s']['on']['events']:
        if 4.8<=event['start_s']<=9:
            axes[1].axvspan(event['start_s'],min(event['end_s'],9),color=colors['on'],alpha=.08)
    blocks=summary['five_second_blocks']
    values=[b['difference']['incremental_mean_power_mW'] for b in blocks]
    axes[2].bar(np.arange(6),values,color='#0f766e',width=.6)
    axes[2].set_xticks(np.arange(6),[f'{b["start_s"]}-{b["end_s"]} s' for b in blocks])
    axes[2].set_ylabel('BP ON minus OFF (mW)')
    axes[2].set_title('Mean-power increment by 5 s block (not repeat trials)',loc='left')
    axes[2].set_ylim(min(0,min(values)*1.3),max(values)*1.35)
    delta=summary['primary_0_to_30_s']['difference']['incremental_mean_power_mW']
    axes[2].axhline(delta,ls='--',color='#0f766e',lw=1,label=f'0-30 s: {delta:.2f} mW')
    for x,y in enumerate(values):
        axes[2].text(x,y+.4,f'{y:.2f}',ha='center')
    axes[2].legend(frameon=False,loc='upper right')
    axes[2].grid(axis='y',alpha=.18)
    fig.savefig(OUTPUT/'power_comparison.png',dpi=180)
    fig.savefig(OUTPUT/'power_comparison.svg')
    plt.close(fig)


def write_report(s):
    primary=s['primary_0_to_30_s']; off=primary['conditions']['off']; on=primary['conditions']['on']; d=primary['difference']
    sens=s['sensitivity_5_to_30_s']; sd=sens['difference']; wide=s['wide_excursions_0_to_30_s']; old=s['earlier_wifi_enabled']
    lines=[
        '# 无 Wi-Fi 血压计算功耗分析','',
        f'统一比较两份记录的前 30 秒，未运行血压计算时平均功率为 **{off["mean_power_W"]*1000:.2f} mW**，运行时为 **{on["mean_power_W"]*1000:.2f} mW**。开启计算的平均功率增量为 **{d["incremental_mean_power_mW"]:.2f} mW（{d["relative_power_increase_pct"]:.2f}%）**，30 秒额外能耗为 **{d["incremental_energy_J"]:.4f} J**。','',
        f'沿用之前的 5–30 秒补充口径，增量为 **{sd["incremental_mean_power_mW"]:.2f} mW（{sd["relative_power_increase_pct"]:.2f}%）**。这与之前带 Wi-Fi 时同区间的 {old["sensitivity_5_to_30_s"]["difference"]["incremental_mean_power_mW"]:.2f} mW 接近。','',
        '本次宽峰呈现持续的功率抬升，符合周期计算负载的表现；原先约 1.9 W 的窄高尖峰在新记录中没有出现。','',
        '## 0–30 秒同区间结果','',
        '| 指标 | 未运行血压计算 | 运行血压计算 | 开启后的变化 |','|---|---:|---:|---:|',
        f'| 平均功率（mW） | {off["mean_power_W"]*1000:.3f} | {on["mean_power_W"]*1000:.3f} | {d["incremental_mean_power_mW"]:+.3f} |',
        f'| 相对基线增幅 | — | — | {d["relative_power_increase_pct"]:+.3f}% |',
        f'| 平均输入电流（mA） | {off["mean_current_A"]*1000:.3f} | {on["mean_current_A"]*1000:.3f} | {d["incremental_mean_current_mA"]:+.3f} |',
        f'| 平均输入电压（V） | {off["mean_voltage_V"]:.4f} | {on["mean_voltage_V"]:.4f} | {on["mean_voltage_V"]-off["mean_voltage_V"]:+.4f} |',
        f'| 30 秒能耗（J） | {off["energy_J"]:.6f} | {on["energy_J"]:.6f} | {d["incremental_energy_J"]:+.6f} |',
        f'| 30 秒能耗（mWh） | {off["energy_mWh"]:.6f} | {on["energy_mWh"]:.6f} | {d["incremental_energy_mWh"]:+.6f} |',
        f'| 记录最大功率（mW） | {off["max_power_W"]*1000:.3f} | {on["max_power_W"]*1000:.3f} | 不等同于真实瞬时峰值 |',
        f'| 功率中位数（mW） | {off["median_power_W"]*1000:.3f} | {on["median_power_W"]*1000:.3f} | — |',
        f'| 功率 P95（mW） | {off["p95_power_W"]*1000:.3f} | {on["p95_power_W"]*1000:.3f} | — |','',
        '平均功率使用原始功率对实际时间的梯形积分除以时长；保留全部尖峰，不用平均电压乘平均电流代替。两次记录分别从各自起点计时，不能作同步逐点相减。','',
        '## 宽峰分析','',
        '用户截图显示低功率区与宽峰交替出现。下图使用本次 CSV 的 4.8–9 秒区间进行核对。为客观描述宽峰，采用 20 ms 移动平均后高于 550 mW 的区间，合并不超过 50 ms 的间隙，仅保留跨度至少 100 ms 的事件。550 mW 高于本次无计算基线的全部记录值；该阈值只用于描述宽峰，未用于删改能量积分。','',
        '| 宽峰特征，0–30 秒 | 未运行血压计算 | 运行血压计算 |','|---|---:|---:|',
        f'| 符合上述定义的宽峰数 | {wide["off"]["count"]} | {wide["on"]["count"]} |',
    ]
    if wide['on']['count']:
        w=wide['on']
        lines += [f'| 宽峰跨度中位数（ms） | — | {w["median_span_ms"]:.1f} |',
                  f'| 宽峰跨度范围（ms） | — | {w["min_span_ms"]:.1f}–{w["max_span_ms"]:.1f} |',
                  f'| 相邻宽峰起点间隔中位数（ms） | — | {w["median_start_interval_ms"]:.1f} |',
                  f'| 各宽峰平均功率的中位数（mW） | — | {w["median_event_mean_power_W"]*1000:.1f} |']
        lines += ['', '截图中 5–9 秒附近的宽峰对应记录：','',
                  '| 宽峰起点（s） | 宽峰终点（s） | 跨度（ms） | 区间平均功率（mW） |','|---:|---:|---:|---:|']
        for event in w['events']:
            if 4.8<=event['start_s']<=9:
                lines.append(f'| {event["start_s"]:.3f} | {event["end_s"]:.3f} | {event["duration_ms"]:.1f} | {event["mean_power_W"]*1000:.1f} |')
    lines += ['',
        '宽峰的周期和数百毫秒跨度与项目每 1 秒推进一个计算窗口、之前另一轮日志约 396 ms 的窗口处理耗时相容，最可能对应特征提取、模型读取/推理和血压后处理期间的 CPU 与存储访问负载。未同步采集 GPIO 或模型日志，因此这里是负载归因推测，不是实测任务起止或模型推理时间。阈值、平滑和测量响应均会影响上述跨度。','',
        '**宽峰比全程平均增量明显是正常的：计算只占每个周期的一部分时间。全程约 13–15 mW 的增量不能解释为计算期间只增加 13–15 mW。** 不能把两个独立记录的最大值之差直接作为模型峰值功率，也不能将某一个宽峰的全部电量当作模型电量，必须考虑同期基线。','',
        f'各宽峰区间平均功率中位数约 {wide["on"]["median_event_mean_power_W"]*1000:.1f} mW，与无计算全程平均基线 {off["mean_power_W"]*1000:.1f} mW 相比约高 {(wide["on"]["median_event_mean_power_W"]-off["mean_power_W"])*1000:.1f} mW。这是两个独立记录的描述性对比，仅帮助理解活动期间的功率尺度，不是同步测得的纯模型功率。','',
        '全 30 秒按上述定义只检测到 14 个明显宽峰，15–25 秒尤为稀少，因此不能将此段录制视为每秒都执行一次相同完整推理的连续满负载测试。`wk_realtime_pipeline.c` 在特征/信号质量检查失败时会提前返回 `WK_RT_SIGNAL_REJECTED`，其他错误也可提前结束流程；这是窄峰和宽峰交替的一种合理解释，尚无本轮同步日志来确认各次状态。14 个宽峰也不等同于 14 次成功预测。若要报告持续执行完整流程的平均功耗，应结合稳定输入及对应日志重新统计。','',
        '## 时段敏感性','',
        '| 区间 | 未运行平均功率（mW） | 运行平均功率（mW） | 平均增量（mW） | 增幅 |','|---|---:|---:|---:|---:|']
    def append_interval(label,result):
        c=result['conditions']; diff=result['difference']
        lines.append(f'| {label} | {c["off"]["mean_power_W"]*1000:.3f} | {c["on"]["mean_power_W"]*1000:.3f} | {diff["incremental_mean_power_mW"]:+.3f} | {diff["relative_power_increase_pct"]:+.3f}% |')
    for b in s['five_second_blocks']:
        append_interval(f'{b["start_s"]}–{b["end_s"]} s',b)
    append_interval('合计 0–30 s',primary)
    append_interval('补充 5–30 s',sens)
    end=s['all_common_0_to_end']['conditions']['off']['end_s']
    append_interval(f'全部共同区间 0–{end:.3f} s',s['all_common_0_to_end'])
    append_interval(f'补充 5–{end:.3f} s',s['all_common_5_to_end'])
    lines += ['', '0–30 秒为与上次一致的主结果。5–30 秒沿用此前的补充口径：算法需要约 5 秒采样窗口，但功率录制起点没有与设备启动/模型启动同步，不能将其直接命名为已验证的稳态区间。完整共同区间用于检查截取前 30 秒是否改变结论。每个 5 秒分段不是一次独立重复实验。','',
        '## 与之前带 Wi-Fi 的结果比较','',
        '| 条件 | 带 Wi-Fi | 无 Wi-Fi |','|---|---:|---:|',
        f'| 未运行计算平均功率，0–30 s（mW） | {old["primary"]["off"]["mean_power_W"]*1000:.3f} | {off["mean_power_W"]*1000:.3f} |',
        f'| 运行计算平均功率，0–30 s（mW） | {old["primary"]["on"]["mean_power_W"]*1000:.3f} | {on["mean_power_W"]*1000:.3f} |',
        f'| 血压计算平均增量，0–30 s（mW） | {old["difference"]["incremental_mean_power_mW"]:.3f} | {d["incremental_mean_power_mW"]:.3f} |',
        f'| 血压计算平均增量，5–30 s（mW） | {old["sensitivity_5_to_30_s"]["difference"]["incremental_mean_power_mW"]:.3f} | {sd["incremental_mean_power_mW"]:.3f} |',
        f'| 未运行计算的最大记录功率（W） | {old["primary"]["off"]["max_power_W"]:.6f} | {off["max_power_W"]:.6f} |',
        f'| 运行计算的最大记录功率（W） | {old["primary"]["on"]["max_power_W"]:.6f} | {on["max_power_W"]:.6f} |',
        '| 超过 1 W 的连续尖峰段数，关闭/开启计算 | 313 / 314 | 0 / 0 |','',
        f'两次会话中系统平均功率下降约 {old["off_mean_power_reduction_mW"]:.2f} mW（关闭计算）与 {old["on_mean_power_reduction_mW"]:.2f} mW（开启计算）。如果跨会话的供电、采集、信号、串口输出及仪器设置一致，这反映禁用 Wi-Fi 及相关任务带来的系统功率下降；不解释为单独射频芯片的精确功耗。','',
        '原先约 102.4 ms 周期的 >1 W 窄高尖峰在禁用 Wi-Fi 后未再出现，进一步支持其主要与 Wi-Fi 活动有关的判断；目前仍未单独改变信标周期，所以尚不能把原来每一个尖峰都归因于信标。血压计算增量在两次会话中接近，说明此前平均增量较小并非简单地被 Wi-Fi 尖峰遮蔽。','',
        '[之前带 Wi-Fi 的报告](../bp_power_20261019/report.md)；[之前的尖峰分析](../bp_power_20261019/spike_analysis.md)。','',
        '## 原始数据与计算核对','',
        '| 文件 | 编码 | 行数 | 原始时长（s） | 记录间隔 |','|---|---|---:|---:|---|']
    for source in s['sources'].values():
        lines.append(f'| {source["filename"]} | {source["encoding"]} | {source["rows"]:,} | {source["duration_s"]:.3f} | 全部 2 ms |')
    lines += ['', '所需字段均为有限数值，时间严格递增且无记录间隔缺口，累计能量未下降。原始时长不同，不能直接相减两份文件的累计总能量。主比较每组含 15,001 个端点记录与 15,000 个时间间隔。原始 CSV 未修改。','',
        '| 0–30 秒能量核对 | 原始功率积分（J） | 累计 Wh 末值减初值换算（J） | 相对差异 |','|---|---:|---:|---:|']
    for label,m in [('未运行',off),('运行',on)]:
        lines.append(f'| {label} | {m["energy_J"]:.6f} | {m["instrument_cumulative_energy_delta_J"]:.6f} | {m["energy_integration_minus_counter_pct"]:+.6f}% |')
    lines += ['',
        f'记录功率与同点电压×电流的最大偏差为 {max(r["max_abs_power_minus_UI_W"] for r in s["sources"].values())*1000:.6f} mW。累计能量与功率可能来自同一仪器，此核对验证计算一致性，不代表独立验证仪器精度。','',
        '平均功率：P̄ = Σ[(Pᵢ+Pᵢ₊₁)/2 × (tᵢ₊₁−tᵢ)] / T。新增平均功率：ΔP̄ = P̄开启−P̄关闭。新增能耗：ΔE = E开启−E关闭。1 Wh = 3600 J。','',
        '## 适用范围','',
        '- 按用户提供的文件名标注关闭/开启血压计算且两组均无 Wi-Fi；功耗归因假设除计算开关外其余条件一致，CSV 本身不能独立验证连接状态或固件版本。',
        '- 结果是测量点处血压计算流程的系统平均增量，包含特征提取、推理和后处理及其附带开销；不等同于纯模型、单核或 3.3 V 供电轨功率。',
        '- 每种状态只有一段独立记录，且仪器精度和带宽未提供，暂不能量化重复性或置信区间。',
        '- 没有同步计算执行次数或成功输出数，不能直接给出单次成功预测的实测能耗，也不能将之前另一轮的运行时间当成本轮测量结果。','',
        '## 图与复核数据','',
        '![无 Wi-Fi 功率对比与宽峰放大](power_comparison.png)','',
        '`summary.json` 保留完整精度、源文件 SHA-256、完整原始记录统计、同区间结果、分段数据与宽峰事件。分析脚本：`tmp/analyze_bp_power_no_wifi.py`。','',
    ]
    (OUTPUT/'report.md').write_text('\n'.join(lines),encoding='utf-8')


if __name__ == '__main__':
    main()
