for D in */; do
	( cd ${D}; ./test.sh; ) &
done

wait
