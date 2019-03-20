#!/usr/bin/env bash

res=$(notmuch new)

if [[ "$res" == "No new mail." ]]; then
  exit 0
fi

for ffolder in $HOME/.mail/princeton/*; do
  folder=${ffolder##*/}
  if [[ $folder == INBOX ]]; then
    notmuch tag -new +inbox +princeton folder:"princeton/$folder" and is:new
  elif [[ $folder == SentItems ]]; then
    notmuch tag -new +sent +princeton folder:"princeton/$folder" and is:new
  elif [[ $folder == DeletedItems ]]; then
    notmuch tag -new +deleted +princeton folder:"princeton/$folder" and is:new
  elif [[ $folder == JunkEMail ]]; then
    notmuch tag -new +junk +princeton folder:"princeton/$folder" and is:new
  else
    notmuch tag -new +princeton folder:"princeton/$folder" and is:new
  fi
done

for ffolder in $HOME/.mail/bnl/*; do
  folder=${ffolder##*/}
  if [[ $folder == INBOX ]]; then
    notmuch tag -new +inbox +bnl folder:"bnl/$folder" and is:new
  elif [[ $folder == SentItems ]]; then
    notmuch tag -new +sent +bnl folder:"bnl/$folder" and is:new
  elif [[ $folder == DeletedItems ]]; then
    notmuch tag -new +deleted +bnl folder:"bnl/$folder" and is:new
  elif [[ $folder == JunkEMail ]]; then
    notmuch tag -new +junk +bnl folder:"bnl/$folder" and is:new
  else
    notmuch tag -new +bnl folder:"bnl/$folder" and is:new
  fi
done

for file in $(notmuch search --output=files is:new); do

  keys=+$(command grep '^X-Keywords:' "$file"|cut -c13-|command sed -r 's/\\//g;s/,/ +/g'|tr '[:upper:]' '[:lower:]')

  id=$(sed -n '/^message-id/I{N;s/^[^<]*<//;s/>.*$//;p;q}' "$file")
  if [[ ${keys} != "+" ]]; then

    if [[ -z "$id" || "$id" == Message-ID* ]]; then
      id=notmuch-sha1-$(sha1sum "$file"|cut -d' ' -f1)
    fi

    if [[ ${keys} == *+package* ]]; then
      notify-send "Package delivered" "$(command grep '^Subject:' $file)"
    elif [[ ${keys} == *+maintenance* ]]; then
      notify-send "Maintenance" "$(command grep '^Subject:' $file)"
    elif [[ ${keys} == *+important* ]]; then
      notify-send "Important!" "$(command grep '^Subject:' $file)"
    fi
    notmuch tag -new +gmail $keys -- id:$id
  else
    notmuch tag -new +gmail -- id:$id
  fi
done
