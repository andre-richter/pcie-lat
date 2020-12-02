#!/usr/bin/python

import sys
import os
import numpy as np
import matplotlib
import matplotlib.mlab as mlab
import matplotlib.pyplot as plt
import subprocess
import traceback 

pci_dev ={
    "name"      : "",
    "loc"       : "",
    "class"     : "",
    "vender"    : "",
    "device"    : "",
    "vd"        : "",
    "isBridge"  : 1,
    "driver"    : ""
        }

def is_root():
    return os.geteuid() == 0

def get_pci_list():
    out = subprocess.Popen(['lspci', '-nm'], 
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT) 
    stdout, stderr = out.communicate()
    lspci_str = stdout.decode('ascii')
    
    pci_list = []
    pcis = lspci_str.split('\n')
    for each_pci in pcis:
        pci = {}
        __ =  each_pci.split(" ")
        if len(__) < 4:
            continue
        pci["loc"]      = __[0].replace('"', '')
        pci["vender"]   = __[2].replace('"', '')
        pci["device"]   = __[3].replace('"', '')
        pci["vd"]       = ":".join([pci["vender"], pci["device"]])
        out = subprocess.Popen(['lspci', '-s', '{}'.format(pci["loc"]), "-mvk"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT)
        stdout, stderr = out.communicate()
        ss = stdout.decode('ascii')
        for line in ss.split("\n"):
            if ': ' in line:
                k, v = line.split(": ")
                if k.strip() == "Class":
                    pci['class'] = v.strip().replace('"', '')
                elif k.strip() == "Vendor":
                    pci['vender'] = v.strip().replace('"', '')
                elif k.strip() == "Device" and ss.split("\n").index(line) > 0:
                    pci['device'] = v.strip().replace('"', '')
                elif k.strip() == "Driver":
                    pci['driver'] = v.strip().replace('"', '')
                else:
                    pass
            else:
                continue
        pci_list.append(pci)
    return pci_list

def print_mach_info(tsc_freq, tsc_overhead, loops):
    print("-------------------------------")
    print("   tsc_freq   : {}".format(tsc_freq))
    print(" tsc_overhead : {} clocks".format(tsc_overhead))
    print("    loops     : {}".format(loops))
    print("-------------------------------")

def clock2ns(clocks, tsc_freq):
    return int(clocks*1000000000/tsc_freq)

def plot_y(y, fname):
    num_width = 10
    ymin = int(min(y))-1
    ymax = int(max(y))+1

    print("Max. and Min. latencies are {}ns  {}ns".format(ymax, ymin))
    margin = max(num_width, 5)
    bins = [ii for ii in range(ymin-margin, ymax+margin, num_width)]

    plt.yscale('log')
    n, bins, patches = plt.hist(y, bins, range=(min(y), max(y)), width=10, color='blue')
    plt.xlabel('nanoseconds')
    plt.ylabel('Probability')
    plt.title('Histogram of PCIe latencies (%s samples)' % len(y))

    plt.savefig(fname, dpi=200, format='png')


def main():
    loops = 0
    if len(sys.argv) < 2:
        print("Usage: {}  [0000]:XX:XX.X [loops]".format(sys.argv[0]))
        exit(-1)
    else:
        pci_test = sys.argv[1]
        if pci_test.startswith('0000:'):
            pci_test = sys.argv[0][5:]
        if len(sys.argv) == 3:
            loops = int(sys.argv[2])
        else:
            loops = 100000
    
    ### must be root to run the script
    if not is_root():
        print("Need root privillege! run as root!")
        exit(-1)

    ### get all devices in this computer
    pcis = get_pci_list()
    if pci_test not in [pp['loc'] for pp in pcis]:
        print("existing PCI devices:")
        for __ in pcis:
            print(__)
        print("{} not found!".format(pci_test))
        exit(-1)

    for p in pcis:
        if p['loc'] == pci_test:
            pci_test = p
    unbind_file = "/sys/bus/pci/devices/0000\:{}/driver/unbind"
    unbind_file = unbind_file.format(pci_test['loc'].replace(':', '\:'))
    if os.path.exists(unbind_file):
        print("Unbind file {} not found!".format(unbind_file))
        exit(-1)
    unbind_ss = 'echo -n "0000:{}" > {}'.format(pci_test['loc'], unbind_file)
    os.system(unbind_ss)

    # insert module
    os.system("make")
    print("finished compiling the pcie-lat, insmod...");
    ins_command = "sudo insmod ./pcie-lat.ko ids={}".format(pci_test['vd'])
    print(ins_command)
    os.system(ins_command)

    # couting
    try:
        sys_path_head = "/sys/bus/pci/devices/0000:{}/pcie-lat/{}/pcielat_"
        sys_path_head = sys_path_head.format(pci_test['loc'], pci_test['loc'])
        tsc_freq        = 0
        tsc_overhead    = 0
        with open(sys_path_head+'tsc_freq', 'r') as __:
            tsc_freq = int(float(__.read()))
        with open(sys_path_head+'tsc_overhead', 'r') as __:
            tsc_overhead = int(float(__.read()))
        with open(sys_path_head+'loops', 'w') as __:
            __.write(str(loops))
        with open(sys_path_head+'target_bar', 'w') as __:
            __.write('0')
        print_mach_info(tsc_freq, tsc_overhead, loops)
        with open(sys_path_head+'measure', 'w') as __:
            __.write('0')
        with open('/dev/pcie-lat/{}'.format(pci_test['loc']), 'rb') as __:
            y = []
            cc = __.read(16)
            while cc:
                acc = 0
                acc2 = 0
                for ii in range(8):
                    acc = acc*256 + int(cc[7-ii])
                    acc2 = acc2*256 + int(cc[15-ii])
                y.append(clock2ns(acc2, tsc_freq))
                # read next 
                cc = __.read(16)
            fname = "pcie_lat_loops{}_{}.png"
            fname = fname.format(loops, pci_test['loc'].replace(':', '..'))
            print("plot the graph")
            plot_y(y, fname)
        
    except Exception:
        traceback.print_exc()
        print("Removing module : sudo rmmod pcie-lat.ko")
        os.system("sudo rmmod pcie-lat.ko")
        exit(-1)


    # remove module
    print("Removing module : sudo rmmod pcie-lat.ko")
    os.system("sudo rmmod pcie-lat.ko")


if __name__ == "__main__":
    main()
