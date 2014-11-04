#!/usr/bin/env ruby
# -*- coding: utf-8 -*-
#
# =============================================================================
#
# Copyright (C) 2014 by the author(s)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# =============================================================================
#
# Author(s):
#   Andre Richter, andre.o.richter @t gmail_com

require 'optparse'

DRIVER_NAME = "pcie-lat"
MODULE      = "/sys/bus/pci/drivers/#{DRIVER_NAME}"

bdf = nil

opts = {
  "target_bar" => 0,
  "bar_offset" => 0,
  "loops"      => 100000
}

parser = OptionParser.new do |options|
  options.banner = "Usage: #{$0} [options]"

  options.on('-p', '--pci B:D:F', 'B:D:F of PCIe device, e.g. 00:05.0') do |_bdf|
    _bdf = _bdf[0..6]
    bdf = _bdf if _bdf =~ /\h\h\:\h\h\.\h/
  end

  options.on('-b', '--BAR n', 'BAR number to read from. Default 0') do |bar|
    bar = bar.to_i
    opts["target_bar"] = bar if (bar >= 0) and (bar < 6)
  end

  options.on('-o', '--offset n', 'Offset within the BAR. Default 0') do |off|
    if off[0..1] == '0x'
      off = off.hex
    else
      off = off.to_i
    end

    opts["bar_offset"] = off
  end

  options.on('-l', '--loops n', 'Measurement loops. Default 1_000_000') do |loops|
    loops = loops.to_i
    opts["loops"] = loops if loops > 0
  end

  options.on('-h', '--help', 'Displays Help') do
    puts options
    exit
  end
end

# Fetch commandline arguments
parser.parse!

unless File.exist?(MODULE)
  puts "Module not loaded."
  exit
end

if bdf.nil?
  puts "No PCIe device specified."
  puts "Do with ./#{$0} -p 00:05.0"
  exit
end

SYSFS_PATH   = "/sys/bus/pci/devices/0000:#{bdf}/#{DRIVER_NAME}/#{bdf}/pcielat_"
CHARDEV_FILE = "/dev/#{DRIVER_NAME}/#{bdf}"

unless File.chardev?(CHARDEV_FILE)
  puts "chardev file does not exist."
  puts "Device bound to #{DRIVER_NAME} driver?"
  exit
end

# Get TSC frequency and measurement routine overhead
# from kernel module
tsc_freq     = File.read(SYSFS_PATH + "tsc_freq").to_f
tsc_overhead = File.read(SYSFS_PATH + "tsc_overhead").to_i

# Write options specified in command line arguments
# to kernel module
opts.each do |key, value|
  unless File.read(SYSFS_PATH + key) == opts[key]
    File.write(SYSFS_PATH + key, opts[key])
  end
end

puts "TSC freq:     #{tsc_freq} Hz"
puts "TSC overhead: #{tsc_overhead} cycles"
puts "Device:       #{bdf}"
puts "BAR:          #{opts["target_bar"]}"
puts "Offset:       0x%x" % opts["bar_offset"]
puts "Loops:        #{opts["loops"]}"
puts ""

# measure
begin
  File.write(SYSFS_PATH + "measure", 1)
rescue => e
  puts "Caught exception: #{e}"
  puts "Maybe 'dmesg' has more info!"
  exit
end

# collect results
res = []
File.open(CHARDEV_FILE) do |f|
  while m = f.read(16)
    res << m.unpack('QQ')
  end
end

# Subtract measurement routine overhead and
# make tsc timestamps relative to first measurement
start = res[0][0]
res.map! { |r| {time: r[0] - start, latency: r[1] - tsc_overhead} }

# Calculate results
def mean(x)
  x.inject(0) { |acc, y| acc + y[:latency] } / x.size.to_f
end

def stdd(x, mean)
  Math.sqrt(x.inject(0) { |acc, y| acc + (y[:latency] - mean)**2 } / (x.size.to_f - 1))
end

mean = mean(res)
stdd = stdd(res, mean)

mean_t = mean / tsc_freq * 1000000000
stdd_t = stdd / tsc_freq * 1000000000

# PCI latency measurements produce rare but huge outliers that
# interfere first and foremost when plotting histograms.
#
# Delete them by using a 3σ intervall of the measurement series
max = (mean + 3*stdd).to_i
min = (mean - 3*stdd).to_i

res.delete_if { |r| r[:latency] >= max or r[:latency] <= min }

# Calculate again
mean3σ = mean(res)
stdd3σ = stdd(res, mean3σ)

mean3σ_t = mean3σ / tsc_freq * 1000000000
stdd3σ_t = stdd3σ / tsc_freq * 1000000000

# Output to stdout
puts  "       | Results (#{opts["loops"]} samples)"
puts  "------------------------------------------------------"
print "Mean   | " + ("%.2f cycles" % mean).rjust(16)
puts  " | " + ("%.2f ns" % mean_t).rjust(12)
print "Stdd   | " + ("%.2f cycles" % stdd).rjust(16)
puts  " | " + ("%.2f ns" % stdd_t).rjust(12)

print "\n\n"

print "       | 3σ Results (#{res.size} samples"
puts  ", %.3f%% discarded)" % ((opts["loops"] - res.size) / opts["loops"].to_f)
puts  "------------------------------------------------------"
print "Mean   | " + ("%.2f cycles" % mean3σ).rjust(16)
puts  " | " + ("%.2f ns" % mean3σ_t).rjust(12)
print "Stdd   | " + ("%.2f cycles" % stdd3σ).rjust(16)
puts  " | " + ("%.2f ns" % stdd3σ_t).rjust(12)

# Write to file
puts "\n" + "writing 3σ values (in ns) to file..."

File.open("lat_#{opts["loops"]}_loops_3sigma.csv", "w") do |f|
  res.each do |r|
    time = r[:time] / tsc_freq * 1000000000
    lat  = r[:latency] / tsc_freq * 1000000000
    f.write("%u,%u\n" % [time, lat])
  end
end
