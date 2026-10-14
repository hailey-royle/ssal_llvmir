for D in */; do
	( cd ${D}; ./test.sh; ) &
done

wait

sort test_results.log
rm test_results.log

