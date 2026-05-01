"""
嵌入式设备自动化联调 Agent 系统
三Agent协作 + 硬件在环架构实现固件调试全闭环
"""

import asyncio
import json
import logging
import time
import re
import subprocess
import threading
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any, Callable
from datetime import datetime
import queue
import hashlib

# 配置日志
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(name)s] %(levelname)s: %(message)s'
)
logger = logging.getLogger("EmbeddedDebugAgent")


# ============ 数据模型定义 ============

class LogLevel(Enum):
    """日志级别"""
    DEBUG = "DEBUG"
    INFO = "INFO"
    WARN = "WARN"
    ERROR = "ERROR"
    FATAL = "FATAL"


class TestResult(Enum):
    """测试结果"""
    PASS = "PASS"
    FAIL = "FAIL"
    TIMEOUT = "TIMEOUT"
    ERROR = "ERROR"


@dataclass
class SerialLog:
    """串口日志条目"""
    timestamp: float
    level: LogLevel
    message: str
    raw: str
    source: str = "UART0"


@dataclass
class GPIOSignal:
    """GPIO信号变化"""
    timestamp: float
    pin: str
    value: int
    direction: str = "input"


@dataclass
class I2CTransaction:
    """I2C通信记录"""
    timestamp: float
    address: int
    data: bytes
    is_read: bool
    ack: bool


@dataclass
class TimingSequence:
    """时序序列定义"""
    name: str
    expected_events: List[Dict[str, Any]]
    tolerance_ms: float = 10.0


@dataclass
class Anomaly:
    """异常记录"""
    type: str
    severity: str
    description: str
    timestamp: float
    context: Dict[str, Any]
    related_code_lines: List[int] = field(default_factory=list)
    fix_suggestion: str = ""


@dataclass
class FirmwareBuild:
    """固件构建信息"""
    version: str
    binary_path: Path
    build_time: float
    commit_hash: str
    target: str


@dataclass
class TestSession:
    """测试会话"""
    session_id: str
    firmware: FirmwareBuild
    test_config: Dict[str, Any]
    start_time: float
    end_time: Optional[float] = None
    result: Optional[TestResult] = None
    anomalies: List[Anomaly] = field(default_factory=list)
    logs: List[SerialLog] = field(default_factory=list)
    gpio_signals: List[GPIOSignal] = field(default_factory=list)


# ============ 硬件抽象层 ============

class HardwareInterface(ABC):
    """硬件接口抽象基类"""
    
    @abstractmethod
    async def connect(self) -> bool:
        """连接硬件"""
        pass
    
    @abstractmethod
    async def disconnect(self) -> None:
        """断开硬件"""
        pass
    
    @abstractmethod
    async def is_connected(self) -> bool:
        """检查连接状态"""
        pass


class PowerSupply(HardwareInterface):
    """程控电源接口"""
    
    def __init__(self, port: str = "/dev/ttyUSB0"):
        self.port = port
        self._connected = False
        self._voltage = 0.0
        self._current_limit = 1.0
        
    async def connect(self) -> bool:
        """连接程控电源"""
        try:
            # 实际场景使用 PyVISA 或串口协议
            logger.info(f"连接程控电源: {self.port}")
            await asyncio.sleep(0.5)
            self._connected = True
            return True
        except Exception as e:
            logger.error(f"电源连接失败: {e}")
            return False
    
    async def disconnect(self) -> None:
        self._connected = False
        logger.info("程控电源已断开")
    
    async def is_connected(self) -> bool:
        return self._connected
    
    async def set_voltage(self, voltage: float) -> bool:
        """设置电压"""
        if not self._connected:
            return False
        self._voltage = voltage
        logger.info(f"设置电压: {voltage}V")
        await asyncio.sleep(0.1)
        return True
    
    async def set_current_limit(self, current: float) -> bool:
        """设置电流限制"""
        self._current_limit = current
        logger.info(f"设置电流限制: {current}A")
        await asyncio.sleep(0.1)
        return True
    
    async def measure_current(self) -> float:
        """测量电流"""
        await asyncio.sleep(0.05)
        return 0.15  # 模拟150mA
    
    async def power_on(self) -> bool:
        """上电"""
        logger.info("设备上电")
        return await self.set_voltage(3.3)
    
    async def power_off(self) -> bool:
        """断电"""
        logger.info("设备断电")
        return await self.set_voltage(0.0)
    
    async def power_cycle(self, delay_ms: int = 1000) -> bool:
        """电源循环"""
        await self.power_off()
        await asyncio.sleep(delay_ms / 1000)
        await self.power_on()
        return True


class USBRelay(HardwareInterface):
    """USB继电器控制"""
    
    def __init__(self, port: str = "/dev/ttyUSB1"):
        self.port = port
        self._connected = False
        self._relay_states = [False] * 8
        
    async def connect(self) -> bool:
        logger.info(f"连接USB继电器: {self.port}")
        self._connected = True
        return True
    
    async def disconnect(self) -> None:
        self._connected = False
    
    async def is_connected(self) -> bool:
        return self._connected
    
    async def set_relay(self, channel: int, state: bool) -> bool:
        """设置继电器状态"""
        if 0 <= channel < 8:
            self._relay_states[channel] = state
            logger.info(f"继电器 CH{channel}: {'闭合' if state else '断开'}")
            await asyncio.sleep(0.02)
            return True
        return False
    
    async def pulse_relay(self, channel: int, duration_ms: int = 100) -> bool:
        """继电器脉冲（模拟按键）"""
        await self.set_relay(channel, True)
        await asyncio.sleep(duration_ms / 1000)
        await self.set_relay(channel, False)
        return True
    
    async def set_all(self, states: List[bool]) -> bool:
        """设置所有继电器"""
        for i, state in enumerate(states[:8]):
            await self.set_relay(i, state)
        return True


class LogicAnalyzer(HardwareInterface):
    """逻辑分析仪接口（如 Saleae）"""
    
    def __init__(self, sample_rate_hz: int = 12_000_000):
        self.sample_rate_hz = sample_rate_hz
        self._connected = False
        self._capturing = False
        self._data_queue = asyncio.Queue()
        
    async def connect(self) -> bool:
        logger.info(f"连接逻辑分析仪 (采样率: {self.sample_rate_hz}Hz)")
        self._connected = True
        return True
    
    async def disconnect(self) -> None:
        await self.stop_capture()
        self._connected = False
    
    async def is_connected(self) -> bool:
        return self._connected
    
    async def start_capture(self, channels: List[int]) -> bool:
        """开始捕获"""
        self._capturing = True
        logger.info(f"开始捕获通道: {channels}")
        return True
    
    async def stop_capture(self) -> bool:
        """停止捕获"""
        self._capturing = False
        logger.info("停止捕获")
        return True
    
    async def get_i2c_data(self) -> List[I2CTransaction]:
        """获取I2C数据"""
        # 模拟I2C数据
        return []
    
    async def get_gpio_changes(self) -> List[GPIOSignal]:
        """获取GPIO变化"""
        return []


class SerialMonitor(HardwareInterface):
    """串口监控器"""
    
    def __init__(self, port: str = "/dev/ttyUSB2", baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self._connected = False
        self._monitoring = False
        self._log_queue = asyncio.Queue()
        self._logs: List[SerialLog] = []
        
    async def connect(self) -> bool:
        """连接串口"""
        logger.info(f"连接串口: {self.port} @ {self.baudrate}")
        # 实际使用 pyserial-asyncio
        self._connected = True
        return True
    
    async def disconnect(self) -> None:
        await self.stop_monitoring()
        self._connected = False
    
    async def is_connected(self) -> bool:
        return self._connected
    
    async def start_monitoring(self) -> bool:
        """开始监控"""
        self._monitoring = True
        self._logs.clear()
        logger.info("开始串口监控")
        return True
    
    async def stop_monitoring(self) -> bool:
        """停止监控"""
        self._monitoring = False
        logger.info("停止串口监控")
        return True
    
    async def get_logs(self) -> List[SerialLog]:
        """获取所有日志"""
        return self._logs.copy()
    
    async def clear_logs(self) -> None:
        """清除日志"""
        self._logs.clear()
    
    async def wait_for_pattern(self, pattern: str, timeout_ms: int = 5000) -> Optional[str]:
        """等待特定日志模式"""
        start = time.time()
        while (time.time() - start) * 1000 < timeout_ms:
            if self._logs:
                for log in reversed(self._logs):
                    if pattern in log.message:
                        return log.message
            await asyncio.sleep(0.1)
        return None
    
    def _simulate_log(self, message: str, level: LogLevel = LogLevel.INFO):
        """模拟接收日志"""
        log = SerialLog(
            timestamp=time.time(),
            level=level,
            message=message,
            raw=f"[{level.value}] {message}",
        )
        self._logs.append(log)


# ============ 硬件代理 (Hardware Agent) ============

class HardwareAgent:
    """硬件代理：负责激励注入、数据采集"""
    
    def __init__(self):
        self.power_supply = PowerSupply()
        self.relay = USBRelay()
        self.logic_analyzer = LogicAnalyzer()
        self.serial_monitor = SerialMonitor()
        self._hardware_config: Dict[str, Any] = {}
        
    async def initialize(self, config: Dict[str, Any]) -> bool:
        """初始化所有硬件"""
        self._hardware_config = config
        
        # 连接所有硬件
        results = await asyncio.gather(
            self.power_supply.connect(),
            self.relay.connect(),
            self.logic_analyzer.connect(),
            self.serial_monitor.connect(),
        )
        
        if all(results):
            logger.info("所有硬件初始化成功")
            return True
        else:
            logger.error("部分硬件初始化失败")
            return False
    
    async def shutdown(self) -> None:
        """关闭所有硬件"""
        await asyncio.gather(
            self.power_supply.disconnect(),
            self.relay.disconnect(),
            self.logic_analyzer.disconnect(),
            self.serial_monitor.disconnect(),
        )
        logger.info("所有硬件已关闭")
    
    async def prepare_device(self) -> bool:
        """准备设备：上电、初始化"""
        # 先断电确保状态
        await self.power_supply.power_off()
        await asyncio.sleep(0.5)
        
        # 设置电源参数
        await self.power_supply.set_voltage(3.3)
        await self.power_supply.set_current_limit(0.5)
        
        # 断开所有继电器
        await self.relay.set_all([False] * 8)
        
        # 上电
        await self.power_supply.power_on()
        await asyncio.sleep(1.0)  # 等待设备启动
        
        # 开始串口监控
        await self.serial_monitor.start_monitoring()
        
        logger.info("设备准备完成")
        return True
    
    async def inject_input(self, input_type: str, params: Dict[str, Any]) -> bool:
        """注入输入激励"""
        if input_type == "button_press":
            channel = params.get("channel", 0)
            duration = params.get("duration_ms", 100)
            await self.relay.pulse_relay(channel, duration)
            logger.info(f"模拟按键: CH{channel}, {duration}ms")
            
        elif input_type == "sensor_trigger":
            # 通过GPIO或I2C模拟传感器触发
            channel = params.get("channel", 1)
            pulse_pattern = params.get("pattern", "high")
            if pulse_pattern == "high":
                await self.relay.set_relay(channel, True)
                await asyncio.sleep(0.5)
                await self.relay.set_relay(channel, False)
            
        elif input_type == "power_glitch":
            # 模拟电源扰动
            await self.power_supply.set_voltage(2.5)  # 欠压
            await asyncio.sleep(0.1)
            await self.power_supply.set_voltage(3.3)
            
        elif input_type == "i2c_stimulus":
            # 模拟I2C设备响应
            logger.info(f"注入I2C激励: {params}")
            
        return True
    
    async def collect_data(self, duration_ms: int = 5000) -> Dict[str, Any]:
        """采集数据：串口日志、GPIO状态、功耗等"""
        logger.info(f"开始数据采集，持续 {duration_ms}ms")
        
        # 采集期间等待
        await asyncio.sleep(duration_ms / 1000)
        
        # 获取采集数据
        logs = await self.serial_monitor.get_logs()
        current = await self.power_supply.measure_current()
        
        collected_data = {
            "timestamp": time.time(),
            "duration_ms": duration_ms,
            "serial_logs": [
                {
                    "timestamp": log.timestamp,
                    "level": log.level.value,
                    "message": log.message,
                }
                for log in logs
            ],
            "power_consumption_ma": current,
            "gpio_events": [],  # 从逻辑分析仪获取
            "i2c_transactions": [],  # 从逻辑分析仪获取
        }
        
        logger.info(f"数据采集完成: {len(logs)} 条日志, 电流: {current*1000:.1f}mA")
        return collected_data
    
    async def get_device_state(self) -> Dict[str, Any]:
        """获取设备当前状态"""
        return {
            "voltage": 3.3,
            "current_ma": await self.power_supply.measure_current() * 1000,
            "uptime_ms": 0,
            "error_flags": [],
        }


# ============ 固件代理 (Firmware Agent) ============

class FirmwareAgent:
    """固件代理：负责编译、烧录"""
    
    def __init__(self, project_path: Path):
        self.project_path = Path(project_path)
        self.build_dir = self.project_path / "build"
        self.current_firmware: Optional[FirmwareBuild] = None
        self.programmer = "openocd"  # 或 jlink
        
    async def build(self, clean: bool = False) -> Optional[FirmwareBuild]:
        """编译固件"""
        logger.info("开始编译固件...")
        
        try:
            if clean:
                # 清理构建
                subprocess.run(
                    ["make", "clean"],
                    cwd=self.project_path,
                    capture_output=True,
                    timeout=30
                )
            
            # 构建
            result = subprocess.run(
                ["make", "-j8"],
                cwd=self.project_path,
                capture_output=True,
                text=True,
                timeout=120
            )
            
            if result.returncode != 0:
                logger.error(f"编译失败:\n{result.stderr}")
                # 解析编译错误
                errors = self._parse_build_errors(result.stderr)
                for error in errors:
                    logger.error(f"  {error['file']}:{error['line']}: {error['message']}")
                return None
            
            logger.info("编译成功")
            
            # 获取固件信息
            binary_path = self.build_dir / "firmware.bin"
            commit_hash = self._get_commit_hash()
            
            firmware = FirmwareBuild(
                version=datetime.now().strftime("%Y%m%d_%H%M%S"),
                binary_path=binary_path,
                build_time=time.time(),
                commit_hash=commit_hash,
                target="STM32F407"  # 从配置文件读取
            )
            
            self.current_firmware = firmware
            return firmware
            
        except subprocess.TimeoutExpired:
            logger.error("编译超时")
            return None
        except Exception as e:
            logger.error(f"编译异常: {e}")
            return None
    
    async def flash(self, firmware: Optional[FirmwareBuild] = None) -> bool:
        """烧录固件"""
        if firmware is None:
            firmware = self.current_firmware
        
        if firmware is None:
            logger.error("没有可烧录的固件")
            return False
        
        logger.info(f"开始烧录固件: {firmware.binary_path}")
        
        try:
            # OpenOCD 烧录命令
            openocd_cmd = [
                "openocd",
                "-f", "interface/jlink.cfg",
                "-f", "target/stm32f4x.cfg",
                "-c", f"program {firmware.binary_path} verify reset exit"
            ]
            
            result = subprocess.run(
                openocd_cmd,
                capture_output=True,
                text=True,
                timeout=60
            )
            
            if result.returncode == 0 and "verified OK" in result.stdout:
                logger.info("烧录成功")
                return True
            else:
                logger.error(f"烧录失败:\n{result.stderr}")
                return False
                
        except subprocess.TimeoutExpired:
            logger.error("烧录超时")
            return False
        except FileNotFoundError:
            logger.error("未找到 OpenOCD，尝试使用 J-Link Commander")
            return await self._flash_with_jlink(firmware)
        except Exception as e:
            logger.error(f"烧录异常: {e}")
            return False
    
    async def _flash_with_jlink(self, firmware: FirmwareBuild) -> bool:
        """使用J-Link烧录"""
        try:
            # J-Link命令行脚本
            jlink_script = f"""
device STM32F407VG
si SWD
speed 4000
loadbin {firmware.binary_path}, 0x08000000
verifybin {firmware.binary_path}, 0x08000000
r
go
exit
"""
            script_path = self.build_dir / "flash.jlink"
            script_path.write_text(jlink_script)
            
            result = subprocess.run(
                ["JLinkExe", "-device", "STM32F407VG", 
                 "-if", "SWD", "-speed", "4000",
                 "-CommanderScript", str(script_path)],
                capture_output=True,
                text=True,
                timeout=30
            )
            
            return result.returncode == 0
            
        except Exception as e:
            logger.error(f"J-Link烧录失败: {e}")
            return False
    
    def _get_commit_hash(self) -> str:
        """获取Git提交哈希"""
        try:
            result = subprocess.run(
                ["git", "rev-parse", "--short", "HEAD"],
                cwd=self.project_path,
                capture_output=True,
                text=True,
                timeout=5
            )
            return result.stdout.strip()
        except:
            return "unknown"
    
    def _parse_build_errors(self, stderr: str) -> List[Dict[str, Any]]:
        """解析编译错误"""
        errors = []
        # 匹配 GCC 错误格式: file:line:column: error: message
        pattern = r'(.+?):(\d+):(\d+):\s*(error|warning):\s*(.+)'
        for match in re.finditer(pattern, stderr):
            errors.append({
                "file": match.group(1),
                "line": int(match.group(2)),
                "column": int(match.group(3)),
                "type": match.group(4),
                "message": match.group(5).strip()
            })
        return errors
    
    async def get_symbol_table(self) -> Dict[str, int]:
        """获取符号表（用于关联地址到代码行）"""
        symbol_map = {}
        elf_path = self.build_dir / "firmware.elf"
        
        if elf_path.exists():
            try:
                result = subprocess.run(
                    ["arm-none-eabi-nm", "-n", str(elf_path)],
                    capture_output=True,
                    text=True,
                    timeout=10
                )
                for line in result.stdout.splitlines():
                    parts = line.split()
                    if len(parts) >= 3:
                        addr = int(parts[0], 16)
                        name = parts[2]
                        symbol_map[name] = addr
            except:
                pass
        
        return symbol_map


# ============ 分析代理 (Analysis Agent) ============

class AnalysisAgent:
    """分析代理：日志分析、异常检测、代码关联"""
    
    def __init__(self, project_path: Path):
        self.project_path = Path(project_path)
        self.known_patterns: Dict[str, TimingSequence] = {}
        self.error_patterns: List[re.Pattern] = []
        self._load_patterns()
        self._load_error_patterns()
        
    def _load_patterns(self):
        """加载已知时序模式"""
        # 示例：I2C通信时序
        self.known_patterns["i2c_init"] = TimingSequence(
            name="I2C初始化",
            expected_events=[
                {"type": "log", "pattern": "I2C.*Init", "max_delay_ms": 100},
                {"type": "log", "pattern": "I2C.*Ready", "max_delay_ms": 200},
            ],
            tolerance_ms=50
        )
        
        self.known_patterns["sensor_read"] = TimingSequence(
            name="传感器读取",
            expected_events=[
                {"type": "log", "pattern": "Sensor.*Start", "max_delay_ms": 10},
                {"type": "i2c", "address": 0x48, "max_delay_ms": 50},
                {"type": "log", "pattern": "Sensor.*Data.*\\d+", "max_delay_ms": 100},
            ],
            tolerance_ms=20
        )
        
        # 中断响应时间检查
        self.known_patterns["interrupt_response"] = TimingSequence(
            name="中断响应",
            expected_events=[
                {"type": "gpio", "pin": "EXTI0", "value": 1, "max_delay_ms": 0},
                {"type": "log", "pattern": "ISR.*Enter", "max_delay_ms": 5},  # 5us内响应
            ],
            tolerance_ms=1
        )
    
    def _load_error_patterns(self):
        """加载错误模式"""
        self.error_patterns = [
            re.compile(r'HardFault', re.IGNORECASE),
            re.compile(r'BusFault', re.IGNORECASE),
            re.compile(r'MemFault', re.IGNORECASE),
            re.compile(r'Watchdog.*timeout', re.IGNORECASE),
            re.compile(r'Stack.*overflow', re.IGNORECASE),
            re.compile(r'Assertion.*failed', re.IGNORECASE),
            re.compile(r'I2C.*timeout', re.IGNORECASE),
            re.compile(r'SPI.*error', re.IGNORECASE),
            re.compile(r'DMA.*error', re.IGNORECASE),
            re.compile(r'Buffer.*overflow', re.IGNORECASE),
            re.compile(r'(NULL|Null) pointer', re.IGNORECASE),
            re.compile(r'Division by zero', re.IGNORECASE),
        ]
    
    async def analyze_logs(self, 
                          firmware: FirmwareBuild,
                          data: Dict[str, Any],
                          symbol_table: Dict[str, int]) -> List[Anomaly]:
        """分析日志数据，检测异常"""
        anomalies = []
        logs = data.get("serial_logs", [])
        
        logger.info(f"分析 {len(logs)} 条日志...")
        
        # 1. 错误模式匹配
        for log_entry in logs:
            message = log_entry.get("message", "")
            for pattern in self.error_patterns:
                if pattern.search(message):
                    anomaly = Anomaly(
                        type="ERROR_PATTERN",
                        severity="high",
                        description=f"检测到错误模式 '{pattern.pattern}': {message}",
                        timestamp=log_entry.get("timestamp", 0),
                        context={
                            "log_entry": log_entry,
                            "pattern": pattern.pattern
                        }
                    )
                    anomalies.append(anomaly)
        
        # 2. 时序检查
        await self._check_timing_sequences(logs, anomalies)
        
        # 3. 通信协议检查
        await self._check_communication(data, anomalies)
        
        # 4. 资源使用检查
        await self._check_resource_usage(data, anomalies)
        
        # 5. 关联系代码行
        await self._correlate_code(anomalies, symbol_table, firmware)
        
        # 6. 生成修复建议
        for anomaly in anomalies:
            anomaly.fix_suggestion = self._generate_fix_suggestion(anomaly)
        
        logger.info(f"分析完成，发现 {len(anomalies)} 个异常")
        return anomalies
    
    async def _check_timing_sequences(self, 
                                     logs: List[Dict], 
                                     anomalies: List[Anomaly]):
        """检查时序序列"""
        for seq_name, sequence in self.known_patterns.items():
            logger.debug(f"检查时序模式: {seq_name}")
            
            event_times = {}
            for event in sequence.expected_events:
                if event["type"] == "log":
                    pattern = event["pattern"]
                    for log in logs:
                        if re.search(pattern, log.get("message", "")):
                            event_times[id(event)] = log.get("timestamp", 0)
                            break
            
            # 检查事件间的时间间隔
            if len(event_times) >= 2:
                times = sorted(event_times.values())
                for i in range(len(times) - 1):
                    delay = (times[i+1] - times[i]) * 1000  # 转换为ms
                    max_allowed = sequence.expected_events[i+1].get("max_delay_ms", 100)
                    
                    if delay > max_allowed + sequence.tolerance_ms:
                        anomalies.append(Anomaly(
                            type="TIMING_VIOLATION",
                            severity="medium",
                            description=f"时序违规 [{seq_name}]: 事件间隔 {delay:.2f}ms > {max_allowed}ms",
                            timestamp=times[i],
                            context={
                                "sequence": seq_name,
                                "actual_delay_ms": delay,
                                "max_delay_ms": max_allowed,
                                "tolerance_ms": sequence.tolerance_ms
                            }
                        ))
    
    async def _check_communication(self, 
                                  data: Dict[str, Any], 
                                  anomalies: List[Anomaly]):
        """检查通信协议"""
        i2c_transactions = data.get("i2c_transactions", [])
        
        # I2C丢帧检测
        if i2c_transactions:
            for i in range(len(i2c_transactions) - 1):
                if not i2c_transactions[i].get("ack", True):
                    anomalies.append(Anomaly(
                        type="I2C_NACK",
                        severity="high",
                        description=f"I2C通信无应答: 地址 0x{i2c_transactions[i].get('address', 0):02X}",
                        timestamp=i2c_transactions[i].get("timestamp", 0),
                        context={"transaction": i2c_transactions[i]}
                    ))
    
    async def _check_resource_usage(self, 
                                   data: Dict[str, Any], 
                                   anomalies: List[Anomaly]):
        """检查资源使用"""
        # 功耗异常检测
        power_ma = data.get("power_consumption_ma", 0)
        if power_ma > 500:  # 超过500mA可能异常
            anomalies.append(Anomaly(
                type="HIGH_POWER",
                severity="medium",
                description=f"功耗异常: {power_ma:.1f}mA",
                timestamp=data.get("timestamp", 0),
                context={"power_ma": power_ma, "threshold_ma": 500}
            ))
    
    async def _correlate_code(self, 
                             anomalies: List[Anomaly],
                             symbol_table: Dict[str, int],
                             firmware: FirmwareBuild):
        """关联异常到源代码行"""
        for anomaly in anomalies:
            # 从日志消息中提取函数名
            message = anomaly.description
            func_match = re.search(r'in\s+(\w+)\(\)', message)
            if func_match:
                func_name = func_match.group(1)
                if func_name in symbol_table:
                    # 查找函数定义位置
                    code_lines = await self._find_function_lines(func_name)
                    anomaly.related_code_lines = code_lines
                    anomaly.context["function"] = func_name
                    anomaly.context["address"] = hex(symbol_table[func_name])
    
    async def _find_function_lines(self, func_name: str) -> List[int]:
        """查找函数在源文件中的行号"""
        lines = []
        for src_file in self.project_path.rglob("*.c"):
            try:
                content = src_file.read_text(errors='ignore')
                for i, line in enumerate(content.splitlines(), 1):
                    if func_name in line and ('void' in line or 'int' in line):
                        lines.append(i)
            except:
                pass
        return lines
    
    def _generate_fix_suggestion(self, anomaly: Anomaly) -> str:
        """生成修复建议"""
        suggestions = {
            "ERROR_PATTERN": {
                "HardFault": "HardFault异常通常由非法内存访问或未对齐访问引起。检查指针操作和数组边界。",
                "Watchdog": "看门狗超时。检查主循环执行时间或增加喂狗频率。",
                "I2C.*timeout": "I2C通信超时。检查总线是否被拉低、从设备地址是否正确、上拉电阻是否合适。",
                "Stack.*overflow": "栈溢出。增加任务栈大小或检查递归深度。",
                "Assertion.*failed": "断言失败。检查失败条件，添加边界保护。",
                "NULL pointer": "空指针解引用。添加NULL检查或初始化指针。",
            },
            "TIMING_VIOLATION": "时序违规。检查中断优先级、任务调度延迟或优化关键路径代码。",
            "I2C_NACK": "I2C设备无应答。检查设备地址、电源状态和总线连接。",
            "HIGH_POWER": "功耗过高。检查是否有引脚短路、外设未关闭或PWM频率不合理。",
        }
        
        for category, suggestions_map in suggestions.items():
            if anomaly.type == category:
                if isinstance(suggestions_map, dict):
                    for pattern, suggestion in suggestions_map.items():
                        if re.search(pattern, anomaly.description, re.IGNORECASE):
                            return suggestion
                    return "检查相关硬件和软件配置。"
                return suggestions_map
        
        return "需要进一步分析异常原因。"
    
    async def compare_logs(self, 
                          current_logs: List[Dict],
                          baseline_logs: List[Dict]) -> Dict[str, Any]:
        """与基线日志对比"""
        current_set = {log.get("message", "") for log in current_logs}
        baseline_set = {log.get("message", "") for log in baseline_logs}
        
        new_errors = current_set - baseline_set
        fixed_errors = baseline_set - current_set
        
        return {
            "new_errors": list(new_errors),
            "fixed_errors": list(fixed_errors),
            "total_changes": len(new_errors) + len(fixed_errors)
        }


# ============ 主编排器 ============

class DebugOrchestrator:
    """联调编排器：协调三个Agent完成完整的调试循环"""
    
    def __init__(self, project_path: Path, config: Dict[str, Any]):
        self.project_path = Path(project_path)
        self.config = config
        self.hardware_agent = HardwareAgent()
        self.firmware_agent = FirmwareAgent(project_path)
        self.analysis_agent = AnalysisAgent(project_path)
        
        self.sessions: List[TestSession] = []
        self.baseline_logs: List[Dict] = []
        self.max_retries = config.get("max_retries", 3)
        
    async def initialize(self) -> bool:
        """初始化所有Agent"""
        logger.info("初始化联调编排器...")
        
        # 初始化硬件
        hw_ok = await self.hardware_agent.initialize(
            self.config.get("hardware", {})
        )
        if not hw_ok:
            logger.error("硬件初始化失败")
            return False
        
        logger.info("联调编排器初始化完成")
        return True
    
    async def shutdown(self) -> None:
        """关闭系统"""
        await self.hardware_agent.shutdown()
        logger.info("联调编排器已关闭")
    
    async def run_debug_loop(self, 
                            test_sequence: List[Dict[str, Any]],
                            max_cycles: int = 10) -> TestSession:
        """
        运行完整的调试循环
        流程: 编译 -> 烧录 -> 激励 -> 采集 -> 分析 -> (如有问题)修复 -> 重试
        """
        session_id = hashlib.md5(str(time.time()).encode()).hexdigest()[:8]
        logger.info(f"=== 开始调试循环 [{session_id}] ===")
        
        # 1. 编译固件
        firmware = await self.firmware_agent.build(clean=True)
        if firmware is None:
            return self._create_failed_session(session_id, "编译失败")
        
        session = TestSession(
            session_id=session_id,
            firmware=firmware,
            test_config={"test_sequence": test_sequence},
            start_time=time.time()
        )
        
        # 2. 获取符号表
        symbol_table = await self.firmware_agent.get_symbol_table()
        
        # 3. 调试循环
        for cycle in range(max_cycles):
            logger.info(f"\n--- 调试周期 {cycle + 1}/{max_cycles} ---")
            
            # 3a. 烧录固件
            flash_ok = await self.firmware_agent.flash(firmware)
            if not flash_ok:
                logger.error("烧录失败，重试...")
                continue
            
            # 3b. 准备硬件
            await self.hardware_agent.prepare_device()
            
            # 3c. 执行测试序列
            data = await self._execute_test_sequence(test_sequence)
            session.logs = [
                SerialLog(**log) for log in data.get("serial_logs", [])
            ]
            
            # 3d. 分析结果
            anomalies = await self.analysis_agent.analyze_logs(
                firmware, data, symbol_table
            )
            
            if anomalies:
                logger.w