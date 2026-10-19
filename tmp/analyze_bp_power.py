"""Analyze two power-meter CSVs without modifying the source observations."""
import csv
import hashlib
import io
import json
import math
import statistics
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    'off': Path('C:/Users/T/Desktop/未加载血压计算数据.csv'),
    'on': Path('C:/Users/T/Desktop/运行血压数据.csv'),
}
OUTPUT = ROOT / 'reports' / 'bp_power_20261019'


def load(path):
    raw = path.read_bytes()
    for encoding in ('utf-8-sig', 'gb18030'):
        try:
            content = raw.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        raise ValueError('Unsupported CSV encoding: ' + str(path))
    reader = csv.DictReader(io.StringIO(content))
    required = ['电压(V)', '电流(A)', '功率(W)', '能量(Wh)', '时间(ms)']
    if any(key not in reader.fieldnames for key in required):
        raise ValueError('Missing columns: ' + str(reader.fieldnames))
    rows = []
    for line_number, row in enumerate(reader, 2):
        parsed = {key: float(row[key]) for key in required}
        if not all(math.isfinite(value) for value in parsed.values()):
            raise ValueError(f'Nonfinite value at {path}:{line_number}')
        parsed['line'] = line_number
        rows.append(parsed)
    times = [row['时间(ms)'] / 1000 for row in rows]
    if len(rows) < 2 or any(b <= a for a, b in zip(times, times[1:])):
        raise ValueError('Insufficient data or non-increasing timestamps: ' + str(path))
    return rows, {
        'path': str(path), 'filename': path.name, 'encoding': encoding,
        'sha256': hashlib.sha256(raw).hexdigest(), 'rows': len(rows),
        'start_s': times[0], 'end_s': times[-1],
        'duration_s': times[-1] - times[0],
        'dt_ms_counts': dict(Counter(round((b-a)*1000, 6) for a,b in zip(times,times[1:]))),
        'missing_or_nonfinite_rows': 0,
        'max_abs_power_minus_UI_W': max(abs(row['功率(W)']-row['电压(V)']*row['电流(A)']) for row in rows),
        'energy_counter_decreases': sum(b['能量(Wh)'] < a['能量(Wh)'] for a,b in zip(rows, rows[1:])),
    }


def quantile(values, fraction):
    values = sorted(values)
    position = fraction * (len(values)-1)
    lo = int(position)
    hi = min(lo+1, len(values)-1)
    return values[lo] + (position-lo)*(values[hi]-values[lo])


def integrate(rows, key, start, end):
    total = 0.0
    for left, right in zip(rows, rows[1:]):
        a, b = left['时间(ms)']/1000, right['时间(ms)']/1000
        lo, hi = max(start,a), min(end,b)
        if hi <= lo:
            continue
        slope = (right[key]-left[key])/(b-a)
        y0, y1 = left[key] + slope*(lo-a), left[key] + slope*(hi-a)
        total += .5*(y0+y1)*(hi-lo)
    return total


def interpolate(rows, key, at):
    for left, right in zip(rows, rows[1:]):
        a,b = left['时间(ms)']/1000, right['时间(ms)']/1000
        if a <= at <= b:
            return left[key] + (right[key]-left[key])*(at-a)/(b-a)
    raise ValueError('Time outside observations')


def measure(rows, start, end):
    if not rows[0]['时间(ms)']/1000 <= start < end <= rows[-1]['时间(ms)']/1000:
        raise ValueError('Invalid time interval')
    subset = [row for row in rows if start <= row['时间(ms)']/1000 <= end]
    powers = [row['功率(W)'] for row in subset]
    duration = end-start
    energy = integrate(rows, '功率(W)', start, end)
    cumulative = (interpolate(rows,'能量(Wh)',end)-interpolate(rows,'能量(Wh)',start))*3600
    return {
        'start_s': start, 'end_s': end, 'duration_s': duration, 'sample_count': len(subset),
        'mean_power_W': energy/duration,
        'arithmetic_mean_power_W': statistics.mean(powers),
        'mean_voltage_V': integrate(rows,'电压(V)',start,end)/duration,
        'mean_current_A': integrate(rows,'电流(A)',start,end)/duration,
        'min_power_W': min(powers), 'max_power_W': max(powers),
        'median_power_W': statistics.median(powers), 'p95_power_W': quantile(powers,.95),
        'std_power_W_descriptive': statistics.pstdev(powers),
        'energy_J': energy, 'energy_mWh': energy/3.6,
        'instrument_cumulative_energy_delta_J': cumulative,
        'energy_integration_minus_counter_J': energy-cumulative,
        'energy_integration_minus_counter_pct': 100*(energy-cumulative)/cumulative,
    }


def difference(off,on):
    return {
        'incremental_mean_power_W': on['mean_power_W']-off['mean_power_W'],
        'incremental_mean_power_mW': (on['mean_power_W']-off['mean_power_W'])*1000,
        'relative_power_increase_pct': 100*(on['mean_power_W']/off['mean_power_W']-1),
        'incremental_energy_J': on['energy_J']-off['energy_J'],
        'incremental_energy_mWh': on['energy_mWh']-off['energy_mWh'],
        'incremental_mean_current_mA': (on['mean_current_A']-off['mean_current_A'])*1000,
    }


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    datasets, sources = {}, {}
    for condition, path in SOURCES.items():
        datasets[condition], sources[condition] = load(path)
    primary = {key: measure(rows,0,30) for key,rows in datasets.items()}
    full = {key: measure(rows,sources[key]['start_s'],sources[key]['end_s'])
            for key, rows in datasets.items()}
    blocks = []
    for start in range(0,30,5):
        off = measure(datasets['off'],start,start+5)
        on = measure(datasets['on'],start,start+5)
        blocks.append({'start_s':start, 'end_s':start+5, 'off_mean_W':off['mean_power_W'],
                       'on_mean_W':on['mean_power_W'], **difference(off,on)})
    trimmed = {key: measure(rows,5,30) for key,rows in datasets.items()}
    summary = {
        'user_confirmed_only_bp_calculation_switch_changed': True,
        'primary_interval_s': [0,30], 'sources': sources, 'primary': primary,
        'difference': difference(primary['off'],primary['on']),
        'full_record': full, 'five_second_blocks':blocks,
        'sensitivity_5_to_30s':{'conditions':trimmed, 'difference':difference(trimmed['off'],trimmed['on'])},
        'methods': {
            'power_average':'Trapezoidal time integral of recorded power divided by duration',
            'energy':'Integral of recorded power (W) over actual timestamps (s)',
            'primary_duration':'Identical first 30.000 s from both independent recordings',
            'counter_check':'Difference of interpolated cumulative Wh values times 3600',
            'delta':'BP enabled minus disabled; not a pointwise time-synchronized subtraction',
        },
        'limitations':[
            'One recording per condition; high-rate samples are not independent replicate experiments.',
            'CSV timestamp spacing is not proof of instrument measurement bandwidth or accuracy.',
            'Measurement boundary is the power meter input/output location; not isolated MCU core or model-only electrical power.',
            'Includes feature extraction and BP pipeline plus any downstream effects of enabling it.',
            'No simultaneous inference logs/counts; per-successful-prediction energy cannot be measured from these files.',
        ],
    }
    (OUTPUT/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    write_report(summary)
    plot(datasets, summary)
    print(json.dumps({'report':str(OUTPUT/'report.md'), 'difference':summary['difference'],
                      'sensitivity_5_to_30s': summary['sensitivity_5_to_30s']['difference']},ensure_ascii=False,indent=2))


def write_report(summary):
    off,on = summary['primary']['off'],summary['primary']['on']
    delta = summary['difference']
    sensitivity = summary['sensitivity_5_to_30s']
    lines = [
        '# 血压计算功耗实测分析', '',
        f'在两次测试仅切换血压计算开关、其余设置相同的条件下（用户确认），统一比较记录的前 30 秒：开启计算后测量点平均功率增加 **{delta["incremental_mean_power_mW"]:.2f} mW**，相对未开启状态增加 **{delta["relative_power_increase_pct"]:.2f}%**。30 秒新增能耗为 **{delta["incremental_energy_J"]:.4f} J**。', '',
        '这里测得的是血压计算流程引入的系统功率增量，包含特征提取、模型推理、血压处理及开关该流程引起的附带开销；不能进一步分离为纯模型或单独 MCU 内核的电功率。', '',
        '## 同时长比较：0–30 秒', '',
        '| 指标 | 未运行血压计算 | 运行血压计算 | 开启后变化 |',
        '|---|---:|---:|---:|',
        f'| 时间加权平均功率（mW） | {off["mean_power_W"]*1000:.3f} | {on["mean_power_W"]*1000:.3f} | +{delta["incremental_mean_power_mW"]:.3f} |',
        f'| 相对基线功率增幅 | — | — | +{delta["relative_power_increase_pct"]:.3f}% |',
        f'| 平均电流（mA） | {off["mean_current_A"]*1000:.3f} | {on["mean_current_A"]*1000:.3f} | +{delta["incremental_mean_current_mA"]:.3f} |',
        f'| 平均电压（V） | {off["mean_voltage_V"]:.4f} | {on["mean_voltage_V"]:.4f} | {on["mean_voltage_V"]-off["mean_voltage_V"]:+.4f} |',
        f'| 30 秒能耗（J） | {off["energy_J"]:.6f} | {on["energy_J"]:.6f} | +{delta["incremental_energy_J"]:.6f} |',
        f'| 30 秒能耗（mWh） | {off["energy_mWh"]:.6f} | {on["energy_mWh"]:.6f} | +{delta["incremental_energy_mWh"]:.6f} |',
        f'| 采样值最大功率（mW） | {off["max_power_W"]*1000:.3f} | {on["max_power_W"]*1000:.3f} | 不用于推算模型峰值 |',
        f'| 采样功率中位数（mW） | {off["median_power_W"]*1000:.3f} | {on["median_power_W"]*1000:.3f} | — |',
        f'| 采样功率 P95（mW） | {off["p95_power_W"]*1000:.3f} | {on["p95_power_W"]*1000:.3f} | — |',
        f'| 样本点数（含两端） | {off["sample_count"]:,} | {on["sample_count"]:,} | — |', '',
        '主结果使用 0–30 秒原始功率，不删除尖峰、不平滑后求均值，也不将平均电压乘平均电流代替平均功率。两组为独立录制，时间轴从各自记录起点对齐，不代表同步采样。', '',
        '## 时段敏感性', '',
        '| 分析区间 | 未运行平均功率（mW） | 运行平均功率（mW） | 差值（mW） | 增幅 |',
        '|---|---:|---:|---:|---:|',
    ]
    for block in summary['five_second_blocks']:
        lines.append(f'| {block["start_s"]}–{block["end_s"]} s | {block["off_mean_W"]*1000:.3f} | {block["on_mean_W"]*1000:.3f} | {block["incremental_mean_power_mW"]:+.3f} | {block["relative_power_increase_pct"]:+.3f}% |')
    trimmed = sensitivity['conditions']
    td = sensitivity['difference']
    lines += [f'| 合计 5–30 s | {trimmed["off"]["mean_power_W"]*1000:.3f} | {trimmed["on"]["mean_power_W"]*1000:.3f} | {td["incremental_mean_power_mW"]:+.3f} | {td["relative_power_increase_pct"]:+.3f}% |', '',
              '前 5 秒平均功率差只有约 0.42 mW。当前算法需要约 5 秒采样窗口才能开始计算，这与前段差异较小相容，但 CSV 没有同步模型日志，不能确认录制起点对应设备启动或计算启动。因此以全 30 秒为主结果，5–30 秒仅作补充，不直接命名为已验证的稳态功耗。5 秒分段也不是独立重复实验。', '',
              '## 原始文件与核对', '',
              '| 文件 | 原始行数 | 实际记录时长（s） | 时间间隔 |', '|---|---:|---:|---:|']
    for key in ('off','on'):
        source = summary['sources'][key]
        lines.append(f'| {source["filename"]} | {source["rows"]:,} | {source["duration_s"]:.3f} | 全部 2 ms |')
    lines += ['', '两份文件均以 GB18030 编码导出，所需字段无缺失/非数值、时间严格递增、没有重复时间或时间缺口。CSV 时间间隔对应 500 个记录点/秒，仪器实际模拟带宽和更新速率未提供。', '',
              '由于原始记录时长不相等，不能直接相减两份文件的累计总能量来归因于模型。统一 30 秒积分使用 15,001 个端点样本与 15,000 个时间间隔。', '',
              '| 0–30 秒能耗核对 | 功率曲线积分（J） | 仪器累计能量差（J） | 相对差异 |', '|---|---:|---:|---:|']
    for label,metrics in [('未运行',off),('运行',on)]:
        lines.append(f'| {label} | {metrics["energy_J"]:.6f} | {metrics["instrument_cumulative_energy_delta_J"]:.6f} | {metrics["energy_integration_minus_counter_pct"]:+.6f}% |')
    lines += ['', '累计能量列在起点已非零，所以核对时使用末值减初值。能量列可能与功率列来自同一仪器积分，此核对验证计算一致性，不代表独立校准了仪器精度。记录功率与同点 U×I 的最大差约 0.0052 mW，与导出小数精度相容。', '',
              '按全部原始时长计算，未运行和运行的平均功率分别为 '
              f'{summary["full_record"]["off"]["mean_power_W"]*1000:.3f} mW、{summary["full_record"]["on"]["mean_power_W"]*1000:.3f} mW，均值差约 '
              f'{(summary["full_record"]["on"]["mean_power_W"]-summary["full_record"]["off"]["mean_power_W"])*1000:.3f} mW，与等长主结果接近。', '',
              '## 计算方法与适用范围', '',
              r'平均功率：P̄ = Σ[(Pᵢ + Pᵢ₊₁)/2 × (tᵢ₊₁ − tᵢ)] / T。', '',
              '新增平均功率：ΔP̄ = P̄开启 − P̄关闭；新增能耗：ΔE = ΔP̄ × 30 s；1 Wh = 3600 J。', '',
              '- 用户已确认两次测试使用相同供电、人体信号连接和采集/通信设置，仅切换血压计算。',
              '- 功率测量点的具体位置、仪器型号及精度未提供，报告保留测量点口径，不折算为芯片 3.3 V 供电轨功率。',
              '- 每个状态只有一段记录。约 1.86% 是本次差异，尚无重复试验和仪器精度信息来量化测量不确定度。',
              '- 没有这两段功率记录对应的模型执行/成功输出计数，不能准确给出每次成功血压计算的能耗；不能套用上午另一轮日志中的成功率或耗时。', '',
              '## 可引用表述', '',
              f'> 在相同采集与通信配置下，分别关闭和开启血压计算流程进行功率测试。对两组前 30 秒记录积分得到平均功率分别为 {off["mean_power_W"]*1000:.2f} mW 和 {on["mean_power_W"]*1000:.2f} mW。开启血压计算引入的平均功率增量为 {delta["incremental_mean_power_mW"]:.2f} mW（{delta["relative_power_increase_pct"]:.2f}%），对应 30 秒额外能耗 {delta["incremental_energy_J"]:.4f} J。', '',
              '## 图与可复核数据', '',
              '![功率对比曲线](power_comparison.png)', '',
              '`summary.json` 保存未舍入统计值、分段结果、源文件路径和 SHA-256。原始 CSV 保持不变；分析脚本为项目 tmp/analyze_bp_power.py。', '']
    if (OUTPUT/'spike_analysis.md').exists():
        lines += ['## 功率尖峰补充分析', '',
                  '[查看尖峰周期、可能来源与验证方法](spike_analysis.md)。', '']
    (OUTPUT/'report.md').write_text('\n'.join(lines),encoding='utf-8')


def plot(datasets, summary):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np

    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'axes.spines.top':False,
                         'axes.spines.right':False,'svg.fonttype':'none'})
    fig, axes = plt.subplots(3,1,figsize=(11,9),gridspec_kw={'height_ratios':[1.1,1.1,1]},layout='constrained')
    colors = {'off':'#475569','on':'#be123c'}
    names = {'off':'BP calculation OFF','on':'BP calculation ON'}
    for key,rows in datasets.items():
        selected = [row for row in rows if row['时间(ms)'] <= 30000]
        t = np.array([row['时间(ms)']/1000 for row in selected])
        p = np.array([row['功率(W)']*1000 for row in selected])
        axes[0].plot(t,p,color=colors[key],lw=.5,alpha=.65,label=names[key])
        # Visualization only: a 100 ms moving average; numerical results use raw records.
        smooth = np.convolve(p,np.ones(50)/50,mode='valid')
        smooth_t = np.convolve(t,np.ones(50)/50,mode='valid')
        axes[1].plot(smooth_t,smooth,color=colors[key],lw=1,label=names[key])
    axes[0].set_title('Recorded power, identical first 30 s (independent runs)',loc='left',fontweight='bold')
    axes[0].set_ylabel('Power (mW)')
    axes[0].legend(loc='upper right',frameon=False,ncols=2)
    axes[1].set_title('100 ms moving average (display only)',loc='left')
    axes[1].set_ylabel('Power (mW)')
    for ax in axes[:2]:
        ax.set_xlim(0,30)
        ax.set_xlabel('Elapsed time from each recording start (s)')
        ax.grid(axis='y',alpha=.18)
    blocks = summary['five_second_blocks']
    x = np.arange(len(blocks))
    axes[2].bar(x,[row['incremental_mean_power_mW'] for row in blocks],color='#0f766e',width=.65)
    axes[2].set_xticks(x,[f'{row["start_s"]}-{row["end_s"]} s' for row in blocks])
    axes[2].set_ylabel('ON minus OFF (mW)')
    axes[2].set_ylim(0,max(row['incremental_mean_power_mW'] for row in blocks)*1.28)
    axes[2].set_title('Mean-power difference by 5 s block (not repeat trials)',loc='left')
    axes[2].axhline(summary['difference']['incremental_mean_power_mW'],color='#0f766e',ls='--',lw=1,
                       label=f'0-30 s mean difference: {summary["difference"]["incremental_mean_power_mW"]:.2f} mW')
    for pos,row in zip(x,blocks):
        axes[2].text(pos,row['incremental_mean_power_mW']+.4,f'{row["incremental_mean_power_mW"]:.2f}',ha='center')
    axes[2].legend(loc='upper right',frameon=False)
    axes[2].grid(axis='y',alpha=.18)
    fig.savefig(OUTPUT/'power_comparison.png',dpi=180)
    fig.savefig(OUTPUT/'power_comparison.svg')
    plt.close(fig)


if __name__ == '__main__':
    main()
