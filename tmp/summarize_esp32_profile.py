"""Summarize measured device logs into a Chinese report and traceable JSON."""
import argparse
import hashlib
import json
import re
import shutil
import statistics
from collections import Counter
from pathlib import Path

from capture_esp32_profile import printable_lines


def key_values(line):
    values = {}
    for key, value in re.findall(r'(\w+)=([^\s]+)', line):
        try:
            values[key] = int(value)
        except ValueError:
            try:
                values[key] = float(value)
            except ValueError:
                values[key] = value
    return values


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def distribution(values):
    return dict(count=len(values), mean=statistics.mean(values),
                minimum=min(values), median=statistics.median(values),
                p95=percentile(values, .95), maximum=max(values)) if values else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    parser.add_argument('--startup-probe', type=Path, required=True)
    parser.add_argument('--previous-summary', type=Path)
    args = parser.parse_args()
    folder = args.folder.resolve()
    root = Path(__file__).resolve().parents[1]
    metadata = json.loads((folder / 'capture_metadata.json').read_text(encoding='utf-8'))
    shutil.copyfile(args.startup_probe, folder / 'initial_probe_raw.bin')
    startup = list(printable_lines(args.startup_probe.read_bytes()))
    (folder / 'initial_probe_log.txt').write_text('\n'.join(startup) + '\n', encoding='utf-8')
    resources = {}
    for line in startup:
        if 'WK_RT: RESOURCE ' in line:
            resources.update(key_values(line))
    lines = (folder / 'device_log.txt').read_text(encoding='utf-8').splitlines()
    current_resources = {}
    for line in lines:
        if 'WK_RT: RESOURCE ' in line:
            current_resources.update(key_values(line))
    resources.update(current_resources)
    resource_source = ('current_capture' if current_resources else
                       ('previous_session_startup_log' if args.previous_summary else 'initial_probe'))
    windows = []
    latest = None
    other_errors = []
    warnings = Counter()
    for line_number, line in enumerate(lines, 1):
        if 'WK_RT: PROFILE ' in line:
            latest = key_values(line)
            latest['device_log_ms'] = int(re.search(r'\((\d+)\)', line).group(1))
            latest['source_line'] = line_number
            windows.append(latest)
        elif 'WK_RT: RAM ' in line and latest is not None:
            latest.update(key_values(line))
        elif 'WK_RT: BP ' in line and latest is not None:
            # BP values are retained as raw evidence, not clinical validation.
            latest['output_log_present'] = True
        elif 'WK_RT: window rejected:' in line:
            reason = re.search(r'window rejected: (\w+) \(([^)]+)\)', line)
            if reason:
                warnings[reason.group(1) + ':' + reason.group(2)] += 1
                if latest is not None:
                    latest['rejection_reason'] = reason.group(2)
        elif 'pipeline error:' in line:
            other_errors.append({'source_line': line_number, 'log': line})
    if not windows:
        raise RuntimeError('No PROFILE records; cannot claim measured timing.')
    required = ('status', 'n', 'sample', 'time_us', 'avg_us', 'max_us', 'over_hop', 'latency_us', 'queue')
    incomplete = []
    complete = []
    for window in windows:
        missing = [key for key in required if key not in window]
        if missing:
            incomplete.append({'source_line': window['source_line'], 'missing_fields': missing,
                               'fields': window})
        else:
            complete.append(window)
    windows = complete
    if not windows:
        raise RuntimeError('No complete PROFILE records.')
    duplicates = len(windows) - len({(row['device_log_ms'], row['sample']) for row in windows})
    if duplicates:
        raise RuntimeError(f'Duplicate PROFILE records: {duplicates}')
    statuses = {}
    for status in sorted({row['status'] for row in windows}):
        subset = [row for row in windows if row['status'] == status]
        statuses[status] = {
            'windows': len(subset),
            'processing_ms': distribution([row['time_us'] / 1000 for row in subset]),
            'last_sample_to_completion_ms': distribution([row['latency_us'] / 1000 for row in subset]),
            'queue_samples': distribution([row['queue'] for row in subset]),
            'calls_over_1s_observed': sum(row['time_us'] > 1000000 for row in subset),
            'firmware_lifetime_last_n': subset[-1]['n'],
            'firmware_lifetime_last_avg_us': subset[-1]['avg_us'],
            'firmware_lifetime_last_max_us': subset[-1]['max_us'],
            'firmware_lifetime_last_over_hop': subset[-1]['over_hop'],
            'counter_gaps': sum(max(0, b['n'] - a['n'] - 1) for a, b in zip(subset, subset[1:])),
        }
    memory_keys = ('stack_peak_since_start_B', 'stack_reserved_B',
                   'internal_used_peak_bound_B', 'psram_used_peak_bound_B',
                   'internal_min_free_B', 'psram_min_free_B',
                   'internal_extra_peak_bound_B', 'psram_extra_peak_bound_B')
    memory = {}
    for key in memory_keys:
        observed = [row[key] for row in windows if key in row]
        memory[key] = {'records': len(observed), 'minimum': min(observed), 'maximum': max(observed)} if observed else None
    drop_patterns = ('sample queue full', 'dropping', 'overflow', 'queue full', 'sample-drop', 'lost', 'missed')
    drop_logs = [{'source_line': i, 'log': line} for i, line in enumerate(lines, 1)
                 if any(pattern in line.lower() for pattern in drop_patterns)]
    discontinuities = [{'previous_sample': a['sample'], 'next_sample': b['sample']}
                       for a, b in zip(windows, windows[1:]) if b['sample'] - a['sample'] != 250]
    resets = [line for line in lines if 'esp_restart' in line or 'boot:' in line]
    startup_resource_lines = [line for line in lines if 'WK_RT: RESOURCE ' in line]
    config = (root / 'sdkconfig').read_text(encoding='utf-8')
    frequency = re.search(r'^CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=(\d+)$', config, re.M)
    byte_fields = ('model_flash_B', 'partition_reserved_B', 'pipeline_psram_B',
                   'edge_state_included_B', 'queue_payload_B', 'task_stack_reserved_B')
    for field in byte_fields:
        if field not in resources:
            raise RuntimeError('Missing startup RESOURCE field: ' + field)
    persistent_subtotal = sum(resources[key] for key in ('pipeline_psram_B', 'queue_payload_B', 'task_stack_reserved_B'))
    summary = {
        'capture': metadata, 'resources_from_initial_device_log': resources,
        'resource_source': resource_source,
        'startup_probe_source_path': str(args.startup_probe.resolve()),
        'configured_cpu_mhz_not_independently_measured': int(frequency.group(1)) if frequency else None,
        'window_count': len(windows), 'status_statistics': statuses,
        'excluded_incomplete_profile_records': incomplete,
        'memory_bounds_bytes': memory, 'known_reserved_payload_subtotal_B': persistent_subtotal,
        'sample_discontinuities': discontinuities, 'drop_warning_lines': drop_logs,
        'restart_indicators_in_capture': resets, 'rejections': dict(warnings),
        'startup_resource_lines_in_capture': startup_resource_lines,
        'pipeline_errors': other_errors,
        'source_sha256': {name: hashlib.sha256((folder / name).read_bytes()).hexdigest()
                          for name in ('serial_raw.bin', 'initial_probe_raw.bin')},
        'limitations': [
            'Processing time includes snapshot, feature extraction, inference, physiological calculation and preemption; excludes prior hop filtering, logs and publication.',
            'Latency is last submitted sample to computation completion, excluding five-second signal acquisition, ADC conversion and transmission.',
            'Heap bounds are system-wide and include other tasks; static RAM outside heap and MMU/cache physical storage are not algorithm-exclusive allocations.',
            'Resource metadata came from initially buffered startup logs; CPU frequency comes from sdkconfig, not independent on-device measurement.',
            'No firmware flash/reset requested; this is not a controlled cold-start benchmark.',
            'No isolated per-model timing, CPU utilization or electrical power was measured.',
        ],
    }
    (folder / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    (folder / 'windows.json').write_text(json.dumps(windows, ensure_ascii=False, indent=2), encoding='utf-8')

    report = [
        '# ESP32 模型资源与运行时间实测报告', '',
        f'- 采集时间：{metadata["started_at"]} 至 {metadata["ended_at"]}（北京时间）。',
        f'- 端口：{metadata["port"]}；串口设置：{metadata["baud"]}；连续采集 {metadata["capture_seconds"]:.2f} 秒。',
        '- 读取现有固件日志；未请求复位、未重新烧录，也未修改业务代码。',
        ('- 输入条件：人体信号已接入（由用户说明；本次不进行血压准确性验证）。'
         if metadata.get('input_condition_user_reported') == 'human_signal_connected'
         else '- 输入条件：见采集记录及用户说明。'),
        f'- 共收到 {len(windows)} 个窗口日志；设备采样序号 {windows[0]["sample"]} 至 {windows[-1]["sample"]}。',
        f'- 按现有固件及先前启动日志，采样率为 250 Hz、窗口 5 秒、步长 1 秒；本次 PROFILE 的采样序号连续性另见下文。当前本地 sdkconfig 配置 CPU 为 {summary["configured_cpu_mhz_not_independently_measured"]} MHz，未独立读取板上实际频率。',
        '', '## 模型大小与固定资源', '',
        ('以下固定资源引用上一次测试的板端 `RESOURCE` 启动日志。本次没有复位设备，也未重新读取模型包头；这些值不属于本次重新测量。若期间改过固件或模型，需要重新核实。'
         if resource_source == 'previous_session_startup_log'
         else '以下来自板端 `RESOURCE` 日志；启动日志时间不计入本次处理时间统计。'), '',
        '| 项目 | 字节 | 换算 | 口径 |', '|---|---:|---:|---|',
    ]
    labels = [
        ('model_flash_B', '已部署模型包', 'MiB', '从板上 WKESP1 包头读取，8 个模型；不含固件代码'),
        ('partition_reserved_B', '模型分区预留容量', 'MiB', '分区容量，不代表模型实际大小'),
        ('pipeline_psram_B', '算法常驻 PSRAM 状态', 'KiB', '包含信号缓冲、特征工作区及 edge 状态'),
        ('edge_state_included_B', '其中：edge 推理状态', 'KiB', '已包含在上一项，不能重复相加'),
        ('queue_payload_B', '模型采样队列数据区', 'KiB', '不含 RTOS 队列控制块、分配器开销'),
        ('task_stack_reserved_B', '模型任务预留栈', 'KiB', '栈实际使用量已包含在预留容量中'),
    ]
    for key, label, unit, note in labels:
        value = resources[key]
        scale = 1048576 if unit == 'MiB' else 1024
        report.append(f'| {label} | {value:,} | {value/scale:.3f} {unit} | {note} |')
    report += [f'| 已知固定分配小计 | {persistent_subtotal:,} | {persistent_subtotal/1024:.3f} KiB | PSRAM 状态 + 队列数据区 + 预留栈；不等于整个固件 RAM |', '',
               f'模型分区剩余容量：{resources["partition_reserved_B"]-resources["model_flash_B"]:,} 字节。模型通过 Flash 映射读取，没有完整复制到 PSRAM；映射窗口大小不等于 RAM 分配量。', '',
               '## 运行时间', '',
               '按本次捕获的每条 `time_us` 重新计算，未把固件启动以来的累计 `avg_us` 当作本次平均值。P95 使用线性插值。', '',
               '| 窗口状态 | 数量 | 平均 ms | 最小 ms | 中位数 ms | P95 ms | 最大 ms | 超过 1 秒 |',
               '|---|---:|---:|---:|---:|---:|---:|---:|']
    for status, stats in statuses.items():
        d = stats['processing_ms']
        report.append(f'| {status} | {stats["windows"]} | {d["mean"]:.3f} | {d["minimum"]:.3f} | {d["median"]:.3f} | {d["p95"]:.3f} | {d["maximum"]:.3f} | {stats["calls_over_1s_observed"]} |')
    successful = [row for row in windows if row['status'] == 'output_ready']
    if successful and successful[0]['n'] == 1:
        later = [row['time_us'] / 1000 for row in successful[1:]]
        summary['first_completed_window_ms'] = successful[0]['time_us'] / 1000
        summary['subsequent_completed_windows_ms'] = distribution(later)
        report += ['', f'本次捕获到设备第一个完成计算的窗口（`n=1`），耗时 {successful[0]["time_us"]/1000:.3f} ms，已保留在总统计中。'
                   + (f'后续 {len(later)} 个完成窗口平均 {statistics.mean(later):.3f} ms。' if later else '')
                   + '首次完成窗口与后续窗口可能执行不同的差分模型路径；没有单独记录各子模型调用耗时。']
    report += ['', '**成功窗口的处理时间涵盖快照、特征提取、模型推理及血压计算，并包含任务抢占；不是单独的树模型推理时间。** 不包含前面 249 个采样点的滤波、日志和网络发布。信号拒绝和错误窗口可能提前返回，不能与成功窗口混合计算模型性能。', '',
               '| 窗口状态 | 末采样到完成平均 ms | P95 ms | 最大 ms | 队列最小/最大（采样点） |',
               '|---|---:|---:|---:|---:|']
    for status, stats in statuses.items():
        d, q = stats['last_sample_to_completion_ms'], stats['queue_samples']
        report.append(f'| {status} | {d["mean"]:.3f} | {d["p95"]:.3f} | {d["maximum"]:.3f} | {q["minimum"]}/{q["maximum"]} |')
    report += ['', '末采样到完成延迟包含排队时间，但不包含 ADC 转换、5 秒采集窗口和结果传输；队列值是窗口处理刚完成时的瞬时积压，不能直接作为持续落后的证据。', '',
               '## 内存观测', '',
               '| 指标 | 本次观测值（字节） | KiB | 解释 |', '|---|---:|---:|---|']
    mem_labels = [
        ('stack_peak_since_start_B', '模型任务栈使用峰值', 'maximum', '自任务启动累计高水位，包括日志/发布；非每窗口独立峰值'),
        ('internal_used_peak_bound_B', '系统内部 RAM 堆占用峰值上界', 'maximum', '包含 Wi-Fi、采集等；不含堆外静态 RAM'),
        ('psram_used_peak_bound_B', '系统 PSRAM 堆占用峰值上界', 'maximum', '系统级堆统计，不是模型独占量'),
        ('internal_min_free_B', '系统内部 RAM 最小空闲', 'minimum', '本次窗口监测的最低值'),
        ('psram_min_free_B', '系统 PSRAM 最小空闲', 'minimum', '本次窗口监测的最低值'),
        ('internal_extra_peak_bound_B', '窗口内部 RAM 额外分配上界', 'maximum', '相对该窗口调用前的空闲量变化'),
        ('psram_extra_peak_bound_B', '窗口 PSRAM 额外分配上界', 'maximum', '不含调用前已分配的常驻状态'),
    ]
    for key, label, direction, note in mem_labels:
        if memory[key]:
            value = memory[key][direction]
            report.append(f'| {label} | {value:,} | {value/1024:.3f} | {note} |')
    report += ['', '内部 RAM 与 PSRAM 的最低空闲可能出现在不同时间，不能相加当作同一时刻精确峰值。常驻状态、栈及系统堆观测存在包含关系，不应把本报告所有 RAM 行相加。', '',
               '## 采集完整性与异常', '',
               f'- 排除字段不完整的 PROFILE 日志：{len(incomplete)} 条（保留在原始日志及 summary.json 中；可能为读取边界的 USB 缓冲残片）。',
               f'- 相邻 PROFILE 采样序号不满足 +250 的次数：{len(discontinuities)}。',
               f'- 按状态累计计数器检测的缺失日志数：{sum(s["counter_gaps"] for s in statuses.values())}。',
               f'- 捕获到的丢样/队列溢出等告警行数：{len(drop_logs)}（未出现告警不等于硬件绝对无丢样）。',
               f'- 捕获到的 pipeline error 日志数：{len(other_errors)}。',
               f'- 本次采集中的启动/重启指示行数：{len(resets)}。']
    report += [f'- `RESOURCE` 启动元数据行数：{len(startup_resource_lines)}；这些行可能由 USB 缓冲保留，不作为重启次数。']
    for reason, count in sorted(warnings.items()):
        report.append(f'- `{reason}`：{count} 个窗口。')
    report += ['', '错误窗口已单列，其耗时不代表成功预测耗时。本次仅记录现有运行状态，未调整信号质量阈值或修复算法错误。', '',
               '## 数据文件和测量边界', '',
               '- `serial_raw.bin`：正式采集的原始串口字节，保留二进制采样帧和文本日志。',
               '- `device_log.txt`：从正式原始数据提取的 ESP-IDF 文本日志。',
               ('- `initial_probe_raw.bin` / `initial_probe_log.txt`：上次探测的补充记录；本次固定资源以 device_log.txt 中重新收到的 RESOURCE 行为准。'
                if resource_source == 'current_capture' else '- `initial_probe_raw.bin` / `initial_probe_log.txt`：RESOURCE 来源记录；复测时引用上次探测文件，未并入本次耗时样本。'),
               '- `windows.json`：逐窗口原始字段与关联 RAM 指标，`source_line` 对应 device_log.txt 行号。',
               '- `summary.json`：统计值、采集信息、完整性检查及原始数据 SHA-256。',
               '- `capture_metadata.json`：正式采集的起止时间及串口参数。', '',
               '未测量功耗、CPU 利用率、单个子模型耗时和上电首次窗口耗时。CPU 主频及代码功能说明来自当前工作区，未读取板上固件哈希来独立确认源码与固件完全一致；板端输出的资源和耗时数据为本次设备日志实测。', '']
    if resource_source == 'previous_session_startup_log':
        summary['limitations'].append('Fixed RESOURCE values are reused from the earlier capture, not newly measured or verified in this run.')
        report[-2] = '未测量功耗、CPU 利用率、单个子模型耗时和上电首次窗口耗时。CPU 主频及代码功能说明来自当前工作区；模型大小与固定资源引用上次记录。未独立确认板上固件哈希。动态堆统计及逐窗口耗时来自本次设备日志。'
    if args.previous_summary:
        previous = json.loads(args.previous_summary.read_text(encoding='utf-8'))
        comparison = {'previous_summary_path': str(args.previous_summary.resolve()),
                      'previous_condition_user_reported': 'no_valid_physiological_signal',
                      'current_condition_user_reported': metadata.get('input_condition_user_reported'),
                      'metrics': []}
        report += ['## 与上次空载测试对比', '',
                   '上次输入条件由用户事后说明为空载；本次为人体信号已接入。两次均为观察性记录，信号质量及其他运行状态不保证相同。', '',
                   '| 指标 | 上次空载 | 本次人体信号接入 |', '|---|---:|---:|']
        metrics = [('完整窗口数', previous['window_count'], len(windows))]
        for status, label in [('output_ready', '完成计算窗口数'),
                              ('signal_rejected', '信号拒绝窗口数'),
                              ('physiology_error', '生理计算错误窗口数'),
                              ('model_error', '模型错误窗口数')]:
            metrics.append((label, previous['status_statistics'].get(status, {}).get('windows', 0),
                            statuses.get(status, {}).get('windows', 0)))
        for key, label in [('mean', '完成计算窗口平均耗时 ms'), ('p95', '完成计算窗口 P95 耗时 ms'),
                           ('maximum', '完成计算窗口最大耗时 ms')]:
            before = previous['status_statistics'].get('output_ready', {}).get('processing_ms', {})
            after = statuses.get('output_ready', {}).get('processing_ms', {})
            metrics.append((label, before.get(key), after.get(key)))
        for key, label in [('internal_used_peak_bound_B', '系统内部 RAM 堆占用峰值上界 B'),
                           ('psram_used_peak_bound_B', '系统 PSRAM 堆占用峰值上界 B'),
                           ('stack_peak_since_start_B', '任务启动以来栈使用峰值 B')]:
            before = previous['memory_bounds_bytes'].get(key) or {}
            after = memory.get(key) or {}
            metrics.append((label, before.get('maximum'), after.get('maximum')))
        def display(value):
            return '无完成计算窗口/未观测' if value is None else (f'{value:.3f}' if isinstance(value, float) else str(value))
        for label, before, after in metrics:
            report.append(f'| {label} | {display(before)} | {display(after)} |')
            comparison['metrics'].append({'metric': label, 'previous': before, 'current': after})
        report += ['', '窗口完成计算不代表血压准确；若本次没有 output_ready，则无法给出人体信号下成功计算的耗时。栈高水位为任务启动以来累计值，不能解释为本次单独达到的栈需求。', '']
        summary['comparison'] = comparison
    (folder / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    (folder / 'report.md').write_text('\n'.join(report), encoding='utf-8')
    print(json.dumps({'report': str(folder / 'report.md'), 'windows': len(windows),
                      'statuses': statuses, 'resources': resources, 'memory': memory}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
