#!/bin/bash

awk 'BEGIN{line=""}{
  if ( $0 ~/(From|To|Cc|Bcc|Subject|Reply-To):/) {
    print
    next
  }
  if($0 ~ / $/){
    line=line$0
  } else {
    print line$0; line=""
  }
}'
