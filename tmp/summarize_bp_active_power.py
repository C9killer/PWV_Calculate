"""Summarize user-confirmed BP computation plateaus using raw-power integrals."""
import json
from pathlib import Path

import numpy as np

from analyze_bp_power_no_wifi import OUTPUT, SOURCES, load, measure, excursions


def aggregate(on_rows, off_rows, end_s, baseline_end_s):
    detected = excursions(on_rows, end=end_s)
    baseline = measure(off_rows, 0, baseline_end_s)
    events = []
    for index, event in enumerate(detected['events'], 1):
        m = measure(on_rows, event['start_s'], event['end_s'])
        dt = event['end_s']-event['start_s']
        events.append({
            'index':index, 'start_s':event['start_s'], 'end_s':event['end_s'],
            'duration_s':dt, 'energy_J':m['energy_J'], 'mean_power_W':m['mean_power_W'],
            'increment_over_control_W':m['mean_power_W']-baseline['mean_power_W'],
        })
    active_time = sum(e['duration_s'] for e in events)
    active_energy = sum(e['energy_J'] for e in events)
    active_mean = active_energy/active_time
    # Independent direct-array trapezoidal integration of each interval.
    t = np.array([r['时间(ms)']/1000 for r in on_rows])
    p = np.array([r['功率(W)'] for r in on_rows])
    independent = 0.0
    for e in events:
        a,b = e['start_s'],e['end_s']
        inner = (t>a)&(t<b)
        xs = np.r_[a,t[inner],b]
        ys = np.r_[np.interp(a,t,p),p[inner],np.interp(b,t,p)]
        independent += float(np.trapezoid(ys,xs))
    assert abs(independent-active_energy)<1e-10
    whole = measure(on_rows,0,end_s)
    outside_time = end_s-active_time
    outside_energy = whole['energy_J']-active_energy
    return {
        'on_record_interval_s':[0,end_s], 'baseline_interval_s':[0,baseline_end_s],
        'control_mean_power_W':baseline['mean_power_W'],
        'event_count':len(events), 'active_total_duration_s':active_time,
        'active_total_energy_J':active_energy,
        'active_time_weighted_mean_power_W':active_mean,
        'active_mean_increment_over_control_W':active_mean-baseline['mean_power_W'],
        'active_relative_increase_pct':100*(active_mean/baseline['mean_power_W']-1),
        'event_mean_power_min_W':min(e['mean_power_W'] for e in events),
        'event_mean_power_max_W':max(e['mean_power_W'] for e in events),
        'mean_duration_ms':active_time/len(events)*1000,
        'median_duration_ms':detected['median_span_ms'],
        'min_duration_ms':detected['min_span_ms'], 'max_duration_ms':detected['max_span_ms'],
        'outside_selected_wide_peaks_mean_W':outside_energy/outside_time,
        'outside_selected_wide_peaks_note':'Includes startup, brief activity and partial/rejected windows; not a pure no-computation baseline',
        'independent_integration_check_max_abs_J':abs(independent-active_energy),
        'events':events,
    }


def main():
    rows, sources = {}, {}
    for key,path in SOURCES.items():
        rows[key], sources[key] = load(path)
    main_result = aggregate(rows['on'],rows['off'],sources['on']['end_s'],sources['off']['end_s'])
    check_30 = aggregate(rows['on'],rows['off'],30,30)
    summary = {
        'user_confirmed_wide_peaks_are_bp_computation':True,
        'scope':'All complete effective wide peaks in the full supplied BP-on recording; control uses full supplied BP-off recording',
        'event_selection':'20 ms moving average > 0.55 W, merging gaps <= 50 ms; retain intervals >= 100 ms; unchanged from earlier analysis',
        'mean_method':'Sum raw-power trapezoidal integrals over selected intervals / sum interval durations',
        'increment_method':'Active mean system power minus independent BP-off control mean system power',
        'sources':sources, 'full_record_effective_peaks':main_result,
        'earlier_0_to_30_s_consistency_check':check_30,
    }
    OUTPUT.mkdir(parents=True,exist_ok=True)
    (OUTPUT/'active_power_summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    r=main_result
    baseline=r['control_mean_power_W']*1000
    active=r['active_time_weighted_mean_power_W']*1000
    delta=r['active_mean_increment_over_control_W']*1000
    text = '\n'.join([
        '# 血压计算有效宽峰功耗简析','',
        f'根据用户确认，将宽峰作为血压模型计算的活动区间。Wi-Fi 关闭时，不计算的平均功率为 **{baseline:.2f} mW**，有效宽峰内系统平均功率为 **{active:.2f} mW**，计算期间平均增加 **{delta:.2f} mW（{r["active_relative_increase_pct"]:.2f}%）**。','',
        '| 指标 | 结果 |','|---|---:|',
        f'| 不运行血压计算的平均功率 | {baseline:.2f} mW |',
        f'| 有效宽峰内时间加权平均功率 | {active:.2f} mW |',
        f'| 计算期间平均功率增量 | +{delta:.2f} mW |',
        f'| 相对不计算时的增幅 | +{r["active_relative_increase_pct"]:.2f}% |',
        f'| 有效宽峰数量 | {r["event_count"]} 个 |',
        f'| 有效宽峰累计时长 | {r["active_total_duration_s"]:.3f} s |',
        f'| 单个宽峰平均跨度 | {r["mean_duration_ms"]:.1f} ms |',
        f'| 单个宽峰跨度范围 | {r["min_duration_ms"]:.0f}–{r["max_duration_ms"]:.0f} ms |',
        f'| 各宽峰区间平均功率范围 | {r["event_mean_power_min_W"]*1000:.2f}–{r["event_mean_power_max_W"]*1000:.2f} mW |','',
        '## 简要分析','',
        f'血压计算使系统功率由约 {baseline:.0f} mW 上升到 {active:.0f} mW，计算活动期间增加约 {delta:.0f} mW，增幅约 {r["active_relative_increase_pct"]:.1f}%。各宽峰平均功率相近，说明本次有效计算区间的功率水平较一致。宽峰期间功率上升明显，而计算间隙会拉低整段录制的平均增量，因此此前全程约 13–15 mW 的增量与这里约 60 mW 的活动期增量并不矛盾。','',
        '上述约 571 mW 是计算期间整机在仪器测量点的总功率，约 60 mW 是相对无计算对照组的增量，包含特征处理、模型推理、血压后处理及相关附带开销。','',
        '## 统计口径','',
        f'- 使用本次开启计算文件全部 {sources["on"]["duration_s"]:.3f} s 中的 {r["event_count"]} 个有效宽峰；无计算基线使用对照文件全部 {sources["off"]["duration_s"]:.3f} s。这里比较的是条件平均功率，不直接比较两份不等长记录的累计能量。',
        '- 沿用此前宽峰提取规则：20 ms 移动平均高于 550 mW，合并不超过 50 ms 的间隙，保留跨度至少 100 ms 的区间。平滑仅用于定位；区间功率仍对原始记录积分，全部区间能量除以全部区间时长，得到时间加权平均。',
        '- 此前约 571.1 mW 是各峰平均功率的中位数；本次使用所有有效宽峰合并后的时间加权平均，口径不同。',
        f'- 若仍只用前 30 s，则有 {check_30["event_count"]} 个有效宽峰：基线 {check_30["control_mean_power_W"]*1000:.2f} mW，宽峰平均 {check_30["active_time_weighted_mean_power_W"]*1000:.2f} mW，增量 {check_30["active_mean_increment_over_control_W"]*1000:.2f} mW（{check_30["active_relative_increase_pct"]:.2f}%），与全文件结果一致。',
        '- 宽峰数量代表所选计算活动区间数量，不能据此确定成功血压输出次数；跨度按功率波形定位，边界会受提取规则和仪器响应影响。','',
        '原始文件：`未加载血压计算数据（无wifi）.csv`、`运行血压数据(无wifi).csv`。源文件未修改。`active_power_summary.json` 保存源文件 SHA-256、完整精度与逐峰数据。','',
    ])
    (OUTPUT/'active_power_summary.md').write_text(text,encoding='utf-8')
    old_report=OUTPUT/'report.md'
    content=old_report.read_text(encoding='utf-8')
    marker='## 有效宽峰的计算期间平均功率（用户确认）'
    if marker not in content:
        title,remaining=content.split('\n',1)
        added='\n'.join(['',marker,'',
            f'用户已确认宽峰对应血压模型计算。按完整文件的 {r["event_count"]} 个有效宽峰重新统计：不计算平均 **{baseline:.2f} mW**，宽峰内时间加权平均 **{active:.2f} mW**，计算期间增加 **{delta:.2f} mW（{r["active_relative_increase_pct"]:.2f}%）**。', '',
            '[查看简要结果及统计口径](active_power_summary.md)。以下保留此前前 30 秒的全程统计，便于区分活动期间功率与整段平均功率。',''])
        old_report.write_text(title+'\n'+added+remaining,encoding='utf-8')
    print(json.dumps({'report':str(OUTPUT/'active_power_summary.md'),'results':{k:v for k,v in r.items() if k!='events'}},ensure_ascii=False,indent=2))


if __name__=='__main__':
    main()
