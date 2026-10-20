#!/usr/bin/env bash
# Exercise the SSH password fallback of tools/midr-deploy inside a throwaway
# container that runs both a test sshd (the host binary) and the client.  The
# test account and its random password exist only in that container, which
# is removed at the end.
#
#   midr-test/deploy-ssh-fallback-test.sh     (needs docker and frr-midr-p6)
set -uo pipefail
TOOLS=${TOOLS:-$(CDPATH= cd -- "$(dirname -- "$0")/../tools/midr-deploy" && pwd -P)}
C=midr-sshtest
docker rm -f $C >/dev/null 2>&1
docker run -d --name $C -v "$TOOLS:/tools:ro" --entrypoint sleep frr-midr-p6:f9beedd0d653 infinity >/dev/null
docker cp /usr/sbin/sshd $C:/usr/sbin/sshd >/dev/null
docker exec -u root -i $C bash -s <<'INNER'
set -uo pipefail
T=/tmp/sshtest; rm -rf $T; mkdir -p $T/profile; chmod 700 $T
export MIDR_SSH_CONTROL_DIR=$T/control
PASS=0; FAIL=0
head -c 18 /dev/urandom | base64 | tr -d '/+=' >$T/pw; chmod 600 $T/pw
PW=$(cat $T/pw)
ok() { echo "[PASS] $*"; PASS=$((PASS + 1)); }
bad() { echo "[FAIL] $*" | sed "s/$PW/<redacted>/g"; FAIL=$((FAIL + 1)); }
show() { tr '\r\n' '  ' <"$1" | sed "s/$PW/<redacted>/g"; }

id sshd >/dev/null 2>&1 || useradd -r -d /run/sshd -s /usr/sbin/nologin sshd
useradd -m -s /bin/bash midrtest
echo "midrtest:$PW" | chpasswd
mkdir -p /run/sshd
ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/test_hostkey
ssh-keygen -q -t ed25519 -N '' -f /root/.ssh/id_ed25519 2>/dev/null || { mkdir -p /root/.ssh; ssh-keygen -q -t ed25519 -N '' -f /root/.ssh/id_ed25519; }
cat >/etc/ssh/sshd_test_config <<'EOF'
Port 22
ListenAddress 127.0.0.1
HostKey /etc/ssh/test_hostkey
UsePAM no
PasswordAuthentication yes
KbdInteractiveAuthentication no
PubkeyAuthentication yes
PermitRootLogin no
EOF
/usr/sbin/sshd -f /etc/ssh/sshd_test_config
cat >$T/profile/nodes.csv <<'EOF'
name,ssh_host,ssh_port,ssh_user,node_id,locator,gateway,edge_bgp,role,group
sshtest,127.0.0.1,22,midrtest,1,2001:db8::2,2001:db8::1,no,member,auto
EOF
printf 'MIDR_SSH_EXTRA="-o StrictHostKeyChecking=no -o UserKnownHostsFile=%s/known_hosts"\n' $T >$T/profile/profile.env
target=midrtest@127.0.0.1
login() { timeout 60 script -qec "/tools/ssh-login.sh --profile $T/profile $*" /dev/null; }
# ssh flushes type-ahead before reading a password: answer each prompt late.
typed() { local x; for x in "$@"; do sleep 3; printf '%s\n' "$x"; done; sleep 3; }
run_remote() {
    ssh -n -o BatchMode=yes -o ControlMaster=no -o "ControlPath=$MIDR_SSH_CONTROL_DIR/%C" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=$T/known_hosts $target "$@"
}
close_master() { ssh -n -o "ControlPath=$MIDR_SSH_CONTROL_DIR/%C" -O exit $target >/dev/null 2>&1 || true; }

echo '== 1. key refused, no terminal'
setsid /tools/ssh-login.sh --profile $T/profile </dev/null >$T/o1 2>&1
grep -q 'run from a terminal to use a password' $T/o1 && ok "no terminal: refused with a hint" || bad "no terminal: $(show $T/o1)"

echo '== 2. key refused, password typed at the prompt'
typed "$PW" | login >$T/o2 2>&1
grep -q 'falling back to a password' $T/o2 && grep -q 'LOGGED IN sshtest' $T/o2 && ok "password fallback logged in" || bad "password fallback: $(show $T/o2)"
[ "$(run_remote whoami 2>/dev/null)" = midrtest ] && ok "plain BatchMode ssh reuses the master" || bad "master not reusable"
. /tools/lib/profile.sh; midr_profile_load $T/profile >/dev/null
[ "$(midr_ssh -n $target whoami 2>/dev/null)" = midrtest ] && ok "midr_ssh works through the master" || bad "midr_ssh failed"
echo hello >$T/f.txt
midr_scp $T/f.txt $target:/tmp/f.txt && [ "$(run_remote cat /tmp/f.txt)" = hello ] && ok "midr_scp works through the master" || bad "midr_scp failed"
start=$SECONDS; v=$(midr_ssh -n $target echo piped)
[ "$v" = piped ] && [ $((SECONDS - start)) -lt 5 ] && ok "midr_ssh inside \$( ) returns at once" || bad "command substitution: '$v' after $((SECONDS - start))s"
printf '' | login >$T/o2b 2>&1
grep -q 'LOGGED IN sshtest' $T/o2b && ! grep -q 'falling back' $T/o2b && ok "second login reuses the master without a prompt" || bad "second login: $(show $T/o2b)"
close_master

echo '== 3. --shared-password'
typed "$PW" | login --shared-password >$T/o3 2>&1
grep -q 'SSH password for the MIDR nodes' $T/o3 && grep -q 'LOGGED IN sshtest' $T/o3 && ok "shared password asked once and logged in" || bad "shared password: $(show $T/o3)"
ls $MIDR_SSH_CONTROL_DIR | grep -q '^password\.' && bad "password file left behind" || ok "password file removed after the run"
close_master

echo '== 4. wrong password'
typed wrong-1 wrong-2 wrong-3 | login >$T/o4 2>&1
grep -q 'password login to .* failed' $T/o4 && grep -q 'FAILED sshtest' $T/o4 && ok "wrong password reported" || bad "wrong password: $(show $T/o4)"

echo '== 5. MIDR_SSH_PASSWORD_FALLBACK=no'
typed "$PW" | MIDR_SSH_PASSWORD_FALLBACK=no login >$T/o5 2>&1
grep -q 'FAILED sshtest' $T/o5 && ! grep -q 'falling back' $T/o5 && ok "fallback disabled" || bad "fallback not disabled: $(show $T/o5)"

echo '== 6. key accepted: no prompt'
mkdir -p ~midrtest/.ssh && cp /root/.ssh/id_ed25519.pub ~midrtest/.ssh/authorized_keys
chown -R midrtest ~midrtest/.ssh && chmod 600 ~midrtest/.ssh/authorized_keys
printf '' | login >$T/o6 2>&1
grep -q 'LOGGED IN sshtest' $T/o6 && ! grep -q 'falling back' $T/o6 && ok "public key used without a prompt" || bad "key login: $(show $T/o6)"
close_master

echo '== 7. connection error is not taken for a key refusal'
sed -i 's/,127.0.0.1,22,/,127.0.0.1,2222,/' $T/profile/nodes.csv
printf '' | login >$T/o7 2>&1
grep -q 'Connection refused' $T/o7 && ! grep -q 'falling back' $T/o7 && ok "connection error reported as is" || bad "unreachable: $(show $T/o7)"
echo "RESULT pass=$PASS fail=$FAIL"
INNER
docker rm -f $C >/dev/null
