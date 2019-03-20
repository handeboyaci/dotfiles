#!/usr/bin/env bash

url="https://forex.1forge.com/1.0.3/quotes?pairs=USDTRY,EURTRY,EURUSD&api_key" 
data="$(curl -s "$url=$(pass 1forge)" | jq '.[]|[.symbol,.price]|@sh')"

echo ${data} | awk 'NR==1{printf("%.3f\n---\n", $2);exit}'
echo "${data}"|tr -d \'|tr -d \"
