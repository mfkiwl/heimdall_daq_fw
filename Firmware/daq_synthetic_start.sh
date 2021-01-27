#!/bin/bash
#
#   DAQ chain start srcipt
#
#   Project : HeIMDALL DAQ Firmware
#   License : GNU GPL V3
#   Authors: Tamas Peto, Carl Laufer

# Read config ini file
en_squelch=$(awk -F "=" '/en_squelch/ {print $2}' daq_chain_config.ini)
out_data_iface_type=$(awk -F "=" '/out_data_iface_type/ {print $2}' daq_chain_config.ini)

# -> Receiver control <-
rec_cfn=_data_control/rec_control_fifo

# -> Sync control <-
sync_cfn=_data_control/sync_control_fifo

# -> Squelch control <-
squelch_cfn=_data_control/squelch_control_fifo

# (re) create control FIFOs
rm _data_control/fw_decimator_in 2> /dev/NULL
rm _data_control/bw_decimator_in 2> /dev/NULL

rm _data_control/fw_decimator_out 2> /dev/NULL
rm _data_control/bw_decimator_out 2> /dev/NULL

rm _data_control/fw_squelch_out 2> /dev/NULL
rm _data_control/bw_squelch_out 2> /dev/NULL

rm _data_control/fw_delay_sync_iq 2> /dev/NULL
rm _data_control/bw_delay_sync_iq 2> /dev/NULL

rm _data_control/fw_delay_sync_hwc 2> /dev/NULL
rm _data_control/bw_delay_sync_hwc 2> /dev/NULL

rm $sync_cfn 2> /dev/NULL
rm $rec_cfn 2> /dev/NULL
rm $squelch_cfn 2> /dev/NULL

mkfifo $sync_cfn
mkfifo $rec_cfn
mkfifo $squelch_cfn

mkfifo _data_control/fw_decimator_in
mkfifo _data_control/bw_decimator_in

mkfifo _data_control/fw_decimator_out
mkfifo _data_control/bw_decimator_out

mkfifo _data_control/fw_squelch_out 2> /dev/NULL
mkfifo _data_control/bw_squelch_out 2> /dev/NULL

mkfifo _data_control/fw_delay_sync_iq
mkfifo _data_control/bw_delay_sync_iq

mkfifo _data_control/fw_delay_sync_hwc
mkfifo _data_control/bw_delay_sync_hwc

# Remove old log files
rm _logs/*.log 2> /dev/NULL

# Useful to set this on low power ARM devices 
#sudo cpufreq-set -g performance

# Set for Tinkerboard with heatsink/fan
#sudo cpufreq-set -d 1.8GHz

# Generating FIR filter coefficients
python3 fir_filter_designer.py

# Start main program chain -Thread 0 Normal (non squelch mode)
echo "Starting DAQ Subsystem with synthetic data source"
python3 _testing/test_data_synthesizer.py 2>_logs/synthetic.log | \
_daq_core/sync.out 2>_logs/sync.log | \
_daq_core/rebuffer.out 0 2> _logs/rebuffer.log &

# Decimator - Thread 1
chrt -f 99 _daq_core/decimate.out 2> _logs/decimator.log &

if [ $en_squelch = 1 ]; then
    _daq_core/squelch.out 0 2> _logs/squelch.log &
fi

# Delay synchronizer - Thread 2
python3 _daq_core/delay_sync.py 2> _logs/delay_sync.log &

# Hardware Controller data path - Thread 3
sudo python3 _daq_core/hw_controller.py 2> _logs/hwc.log &
# root priviliges are needed to drive the i2c master

if [ $out_data_iface_type = eth ]; then
    echo "Output data interface: IQ ethernet server"
    _daq_core/iq_server.out 2>_logs/iq_server.log &
elif [ $out_data_iface_type = shmem ]; then
    echo "Output data interface: Shared memory"
fi

# IQ Eth sink used for testing
#sleep 3
#python3 _daq_core/iq_eth_sink.py 2>_logs/iq_eth_sink.log &

echo -e "      )  (     "
echo -e "      (   ) )  "
echo -e "       ) ( (   "
echo -e "     _______)_ "
echo -e "  .-'---------|" 
echo -e " (  |/\/\/\/\/|"
echo -e "  '-./\/\/\/\/|"
echo -e "    '_________'"
echo -e "     '-------' "
echo -e "               "
echo -e "Have a coffee watch radar"
