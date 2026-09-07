using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Linq;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.Threading;
using LibreHardwareMonitor.Hardware;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
struct HardwareData
{
    public float CpuUsage;   // CPU 사용률 (%)
    public float CpuTemp;    // CPU 온도 (°C)
    public float GpuMemUsedGB; // GPU 메모리 사용량 (GB)
    public float GpuUsage;   // GPU 사용률 (%)
    public float GpuTemp;    // GPU 온도 (°C)
    public float MemUsedGB;  // 메모리 사용량 (GB)
    public float NetInMBs;   // 다운로드 속도 (MB/s)
    public float NetOutMBs;  // 업로드 속도 (MB/s)
}

class Program
{
    const string SHARED_MEM_NAME = "VolumeControlHWDataV2";
    const string MUTEX_NAME = "VolumeControlHWHostMutexV2";
    const int DATA_SIZE = 32;



    [StructLayout(LayoutKind.Sequential)]
    struct MEMORYSTATUSEX
    {
        public uint dwLength;
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX lpBuffer);

    static Computer? computer;
    static long lastNetBytesIn = 0;
    static long lastNetBytesOut = 0;
    static DateTime lastNetTime = DateTime.MinValue;
    static int parentPid = 0;

    static void Main(string[] args)
    {
        // Parse arguments
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--parent" && i + 1 < args.Length && int.TryParse(args[i + 1], out int pid))
            {
                parentPid = pid;
            }
        }

        using var mutex = new Mutex(true, MUTEX_NAME, out bool created);
        if (!created)
        {
            Console.WriteLine("[SysMonitorHost] Already running.");
            return;
        }

        using var mmf = MemoryMappedFile.CreateOrOpen(SHARED_MEM_NAME, DATA_SIZE);
        using var accessor = mmf.CreateViewAccessor(0, DATA_SIZE);

        computer = new Computer
        {
            IsCpuEnabled = true,
            IsGpuEnabled = true,
            IsMemoryEnabled = true,
            IsMotherboardEnabled = true,
            IsControllerEnabled = true,
        };
        try
        {
            computer.Open();
            computer.Accept(new UpdateVisitor());
        }
        catch (Exception ex)
        {
            Console.WriteLine("[SysMonitorHost] LHM Open warning: " + ex.Message);
        }

        Console.WriteLine("[SysMonitorHost] Started successfully. Parent PID: " + parentPid);

        int noParentCheckCount = 0;

        while (true)
        {
            // Check parent lifecycle
            if (parentPid > 0)
            {
                try
                {
                    var parent = Process.GetProcessById(parentPid);
                    if (parent.HasExited)
                    {
                        Console.WriteLine("[SysMonitorHost] Parent process exited. Stopping.");
                        break;
                    }
                }
                catch
                {
                    Console.WriteLine("[SysMonitorHost] Parent process not found. Stopping.");
                    break;
                }
            }
            else
            {
                // If parentPid wasn't supplied, check if volumecontrol process is running
                noParentCheckCount++;
                if (noParentCheckCount >= 10)
                {
                    noParentCheckCount = 0;
                    var procs = Process.GetProcessesByName("volumecontrol");
                    if (procs.Length == 0)
                    {
                        Console.WriteLine("[SysMonitorHost] volumecontrol.exe not running. Stopping.");
                        break;
                    }
                }
            }

            try
            {
                var data = ReadHardwareData();
                accessor.Write(0, ref data);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[SysMonitorHost] Error: " + ex.Message);
            }

            Thread.Sleep(1000);
        }

        try
        {
            computer?.Close();
        }
        catch { }
    }

    static HardwareData ReadHardwareData()
    {
        try
        {
            computer?.Accept(new UpdateVisitor());
        }
        catch { }

        var data = new HardwareData();

        // 1. CPU Usage & Temp from LHM
        float cpuTemp = 0f;
        float cpuUsage = 0f;
        float amdSocTemp = 0f;

        if (computer != null)
        {
            foreach (var hw in computer.Hardware)
            {
                // CPU
                if (hw.HardwareType == HardwareType.Cpu)
                {
                    foreach (var s in hw.Sensors)
                    {
                        float v = s.Value ?? 0f;
                        if (s.SensorType == SensorType.Load && s.Name.Contains("Total"))
                            cpuUsage = v;

                        if (s.SensorType == SensorType.Temperature && v > 0)
                        {
                            if (s.Name.Contains("Package") || s.Name.Contains("Tctl") || s.Name.Contains("Core"))
                            {
                                if (cpuTemp == 0f || s.Name.Contains("Package") || s.Name.Contains("Tctl"))
                                    cpuTemp = v;
                            }
                        }
                    }
                }

                // AMD GPU (internal SoC / iGPU on Ryzen)
                if (hw.HardwareType == HardwareType.GpuAmd)
                {
                    foreach (var s in hw.Sensors)
                    {
                        float v = s.Value ?? 0f;
                        if (s.SensorType == SensorType.Temperature && v > 0)
                        {
                            if (s.Name.Contains("SoC") || s.Name.Contains("VR"))
                                amdSocTemp = v;
                            else if (amdSocTemp == 0f)
                                amdSocTemp = v;
                        }
                    }
                }
            }
        }

        // If CPU temp was 0 from direct CPU sensor (e.g. Zen 4 WinRing0 blocked), fallback to AMD SoC temp
        if (cpuTemp <= 0f && amdSocTemp > 0f)
        {
            cpuTemp = amdSocTemp;
        }

        data.CpuUsage = cpuUsage;
        data.CpuTemp = cpuTemp;



        // 3. GPU (Prefer NVIDIA discrete GPU, fallback to AMD/Intel)
        bool foundNvidia = false;
        if (computer != null)
        {
            foreach (var hw in computer.Hardware)
            {
                if (hw.HardwareType == HardwareType.GpuNvidia)
                {
                    foundNvidia = true;
                    foreach (var s in hw.Sensors)
                    {
                        float v = s.Value ?? 0f;
                        if (s.SensorType == SensorType.Load && (s.Name.Equals("GPU Core", StringComparison.OrdinalIgnoreCase) || s.Name.Contains("Core")))
                            data.GpuUsage = v;
                        if (s.SensorType == SensorType.Temperature && (s.Name.Equals("GPU Core", StringComparison.OrdinalIgnoreCase) || s.Name.Contains("Core")))
                            data.GpuTemp = v;
                        if (s.SensorType == SensorType.SmallData && s.Name.Contains("Memory Used"))
                            data.GpuMemUsedGB = v / 1024f;
                    }
                    break;
                }
            }

            if (!foundNvidia)
            {
                foreach (var hw in computer.Hardware)
                {
                    if (hw.HardwareType == HardwareType.GpuAmd || hw.HardwareType == HardwareType.GpuIntel)
                    {
                        foreach (var s in hw.Sensors)
                        {
                            float v = s.Value ?? 0f;
                            if (s.SensorType == SensorType.Load && s.Name.Contains("Core"))
                                data.GpuUsage = v;
                            if (s.SensorType == SensorType.Temperature && (data.GpuTemp == 0f || s.Name.Contains("Core")))
                                data.GpuTemp = v;
                            if (s.SensorType == SensorType.SmallData && s.Name.Contains("Memory Used"))
                                data.GpuMemUsedGB = v / 1024f;
                        }
                    }
                }
            }
        }

        // 4. Memory via GlobalMemoryStatusEx (100% exact match with Task Manager)
        try
        {
            var ms = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
            if (GlobalMemoryStatusEx(ref ms))
            {
                data.MemUsedGB = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
            }
        }
        catch { }

        // 5. Network Traffic
        var now = DateTime.UtcNow;
        long totalIn = 0, totalOut = 0;
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                var t = ni.NetworkInterfaceType;
                if (t != NetworkInterfaceType.Ethernet && t != NetworkInterfaceType.Wireless80211) continue;

                string desc = ni.Description.ToLowerInvariant();
                if (desc.Contains("virtual") || desc.Contains("vmware") ||
                    desc.Contains("hyper-v") || desc.Contains("loopback") ||
                    desc.Contains("bluetooth") || desc.Contains("miniport") ||
                    desc.Contains("wsl")) continue;

                var stats = ni.GetIPv4Statistics();
                totalIn += stats.BytesReceived;
                totalOut += stats.BytesSent;
            }

            if (lastNetTime != DateTime.MinValue)
            {
                double elapsed = (now - lastNetTime).TotalSeconds;
                if (elapsed > 0)
                {
                    double inMBs = (totalIn - lastNetBytesIn) / elapsed / 1048576.0;
                    double outMBs = (totalOut - lastNetBytesOut) / elapsed / 1048576.0;
                    data.NetInMBs = (float)Math.Max(0, inMBs);
                    data.NetOutMBs = (float)Math.Max(0, outMBs);
                }
            }

            lastNetBytesIn = totalIn;
            lastNetBytesOut = totalOut;
            lastNetTime = now;
        }
        catch { }

        return data;
    }
}

class UpdateVisitor : IVisitor
{
    public void VisitComputer(IComputer c) => c.Traverse(this);
    public void VisitHardware(IHardware h)
    {
        h.Update();
        foreach (var sub in h.SubHardware) sub.Accept(this);
    }
    public void VisitSensor(ISensor s) { }
    public void VisitParameter(IParameter p) { }
}
