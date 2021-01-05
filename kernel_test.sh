#!/bin/bash

start_test() {
echo -e "\t\t\t\t Starting futex Tests" 
cd /mnt/ && make -C tools/testing/selftests TARGETS=futex
cd /mnt/tools/testing/selftests/futex/ && ./run.sh
echo -e "\t\t\t\t Completed futex Tests" 
echo "========================================================================" 
echo -e "\t\t\t\t Starting syscall_user_dispatch:sud_test Tests"
cd /mnt/ && make -C tools/testing/selftests TARGETS=syscall_user_dispatch
cd /mnt/tools/testing/selftests/syscall_user_dispatch && ./sud_test
echo -e "\t\t\t\t Completed syscall_user_dispatch:sud_test Tests"
echo "========================================================================"
echo -e "\t\t\t\t Starting syscall_user_dispatch:sud_benchmark Tests"
./sud_benchmark 
echo -e "\t\t\t\t Completed syscall_user_dispatch:sud_benchmark Tests"
echo "========================================================================" 
}

start_test 2>&1 | tee -a /mnt/kernel_results.log

# Check Interception overhead
MAX_OVERHEAD="10.00"
found_text=$(grep "Interception" /mnt/kernel_results.log)
result=$(echo $found_text | grep -Eo '[0-9]+([.][0-9]+)')
min=$(echo $result $MAX_OVERHEAD | awk '{if ($1 < $2) print "ok"; else print "not_ok"}')
if [ $min == "not_ok" ]; then
echo "Interception overhead greater than 10%" > /mnt/fail.txt
fi

# Parse result file for fail value
grep -q "fail:[1-9]" /mnt/kernel_results.log
ret=$?
if [ $ret -ne 0 ];
then
	touch /mnt/pass.txt
	sync
else
	touch /mnt/fail.txt
	sync
fi
poweroff
