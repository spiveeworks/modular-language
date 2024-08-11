#!/bin/sh

succeeded=0
failed=0
for F in data/*; do
    if tcc -run main.c "$F" > /dev/null
    then
        succeeded=$((succeeded + 1))
    else
        echo "File $F gave an error!"
        failed=$((failed + 1))
    fi
done

if [[ "$failed" = 0 ]]
then
    echo "All $succeeded files in data/ ran successfully!"
else
    echo
    echo "$failed failed, $succeeded succeeded."
fi
