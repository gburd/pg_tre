#!/usr/bin/env bash
# bench/stress/provision-ec2.sh — launch a large local-NVMe EC2 instance and
# prepare it for the pg_tre at-scale stress suite (see STRESS-PLAN.md).
#
# Does, idempotently:
#   1. ensure key pair + security group (SSH from your IP)
#   2. launch an NVMe instance (default i4i.8xlarge), wait for SSH
#   3. RAID-0 the instance-store NVMe volumes into /mnt/nvme (xfs)
#   4. OS tune: THP off, CPU governor performance, swappiness low
#   5. build PostgreSQL (optimized) + pg_tre from a source tarball you scp up
#   6. print the SSH alias and the TERMINATE command
#
# It does NOT run the benchmark — that's stress-suite.sh, run over SSH.
#
# Everything lives under $WORK on the instance; the instance is disposable.
#
# Usage:
#   AWS_PROFILE=mala REGION=us-east-2 ./provision-ec2.sh launch
#   ./provision-ec2.sh terminate      # tears down the instance
#
# Env knobs:
#   PROFILE (default mala)  REGION (us-east-2)  ITYPE (i4i.8xlarge)
#   KEY (pg-tre-bench)  SG_NAME (pg-tre-bench-sg)  PGVER (18.0)
#   NAME (pg-tre-stress)  SSHCFG (/tmp/pgtre-stress-sshcfg)
#   IIDFILE (/tmp/pgtre-stress-iid)  PEM (/tmp/pg-tre-bench.pem)
set -uo pipefail

PROFILE="${PROFILE:-${AWS_PROFILE:-mala}}"
REGION="${REGION:-us-east-2}"
ITYPE="${ITYPE:-i4i.8xlarge}"
KEY="${KEY:-pg-tre-bench}"
SG_NAME="${SG_NAME:-pg-tre-bench-sg}"
PGVER="${PGVER:-18.0}"
NAME="${NAME:-pg-tre-stress}"
SSHCFG="${SSHCFG:-/tmp/pgtre-stress-sshcfg}"
IIDFILE="${IIDFILE:-/tmp/pgtre-stress-iid}"
PEM="${PEM:-/tmp/pg-tre-bench.pem}"
VOL_GB="${VOL_GB:-60}"   # root EBS; data lives on instance-store NVMe

aws() { command aws --profile "$PROFILE" --region "$REGION" "$@"; }
log() { printf '[provision] %s\n' "$*" >&2; }

ensure_keypair_sg() {
    if [ ! -f "$PEM" ] || ! aws ec2 describe-key-pairs --key-names "$KEY" >/dev/null 2>&1; then
        log "creating key pair $KEY -> $PEM"
        aws ec2 create-key-pair --key-name "$KEY" --query KeyMaterial --output text > "$PEM"
        chmod 600 "$PEM"
    fi
    SGID=$(aws ec2 describe-security-groups --group-names "$SG_NAME" \
              --query 'SecurityGroups[0].GroupId' --output text 2>/dev/null || true)
    if [ -z "${SGID:-}" ] || [ "$SGID" = "None" ]; then
        log "creating security group $SG_NAME"
        SGID=$(aws ec2 create-security-group --group-name "$SG_NAME" \
                  --description "pg_tre stress SSH" --query GroupId --output text)
    fi
    MYIP=$(curl -s https://checkip.amazonaws.com)
    aws ec2 authorize-security-group-ingress --group-id "$SGID" \
        --protocol tcp --port 22 --cidr "$MYIP/32" >/dev/null 2>&1 || true
    echo "$SGID"
}

launch() {
    local sgid ami iid ip
    sgid=$(ensure_keypair_sg)
    ami=$(aws ec2 describe-images --owners amazon \
            --filters "Name=name,Values=al2023-ami-2023.*-x86_64" "Name=state,Values=available" \
            --query 'reverse(sort_by(Images,&CreationDate))[0].ImageId' --output text)
    log "launching $ITYPE (ami $ami)"
    iid=$(aws ec2 run-instances --image-id "$ami" --instance-type "$ITYPE" \
            --key-name "$KEY" --security-group-ids "$sgid" \
            --block-device-mappings "[{\"DeviceName\":\"/dev/xvda\",\"Ebs\":{\"VolumeSize\":$VOL_GB,\"VolumeType\":\"gp3\"}}]" \
            --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=$NAME}]" \
            --query 'Instances[0].InstanceId' --output text)
    echo "$iid" > "$IIDFILE"
    log "instance $iid — waiting for running"
    aws ec2 wait instance-running --instance-ids "$iid"
    ip=$(aws ec2 describe-instances --instance-ids "$iid" \
            --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
    cat > "$SSHCFG" <<EOF
Host stress
  HostName $ip
  User ec2-user
  IdentityFile $PEM
  IdentitiesOnly yes
  StrictHostKeyChecking no
  ConnectTimeout 15
  ServerAliveInterval 30
EOF
    log "waiting for SSH ($ip)"
    local i
    for _ in $(seq 1 24); do
        ssh -F "$SSHCFG" stress 'echo ok' </dev/null >/dev/null 2>&1 && break
        sleep 8
    done
    log "SSH up. instance=$iid ip=$ip ssh='ssh -F $SSHCFG stress'"
    setup_nvme
    tune_os
    log "provisioned. Next: scp your pg_tre source + run build_stack, then stress-suite.sh"
    log "TERMINATE with: $0 terminate   (or: aws ec2 terminate-instances --instance-ids $iid)"
}

setup_nvme() {
    log "striping instance-store NVMe into /mnt/nvme"
    ssh -F "$SSHCFG" stress 'sudo bash -s' <<'REMOTE'
set -e
sudo dnf install -y -q mdadm >/dev/null 2>&1 || true
# instance-store NVMe devices are the ones WITHOUT a partition table / not the root.
# On Nitro, the root EBS is /dev/nvme0n1; instance-store are nvme1n1, nvme2n1, ...
mapfile -t STORE < <(lsblk -dn -o NAME,MODEL | awk '/Instance Storage|Amazon EC2 NVMe Instance/{print "/dev/"$1}')
if [ "${#STORE[@]}" -eq 0 ]; then
    # fallback: every nvme?n1 except the one holding the mounted root
    root=$(findmnt -no SOURCE / | sed 's/p\?[0-9]*$//')
    mapfile -t STORE < <(ls /dev/nvme*n1 | grep -v "$root" || true)
fi
echo "instance-store devices: ${STORE[*]:-none}"
if [ "${#STORE[@]}" -ge 2 ]; then
    sudo mdadm --create /dev/md0 --level=0 --raid-devices=${#STORE[@]} "${STORE[@]}" --force
    DEV=/dev/md0
elif [ "${#STORE[@]}" -eq 1 ]; then
    DEV=${STORE[0]}
else
    echo "no instance-store NVMe found; using root disk (SLOW — not representative)"; DEV=""
fi
if [ -n "$DEV" ]; then
    sudo mkfs.xfs -f "$DEV"
    sudo mkdir -p /mnt/nvme
    sudo mount -o noatime,nodiscard "$DEV" /mnt/nvme
    sudo chown ec2-user:ec2-user /mnt/nvme
    df -h /mnt/nvme
else
    sudo mkdir -p /mnt/nvme && sudo chown ec2-user:ec2-user /mnt/nvme
fi
REMOTE
}

tune_os() {
    log "OS tuning (THP off, governor performance)"
    ssh -F "$SSHCFG" stress 'sudo bash -s' <<'REMOTE'
set -e
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null 2>&1 || true
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/defrag  >/dev/null 2>&1 || true
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee "$g" >/dev/null 2>&1 || true; done
sudo sysctl -w vm.swappiness=1 >/dev/null 2>&1 || true
sudo sysctl -w kernel.numa_balancing=0 >/dev/null 2>&1 || true
# deps for PG + pg_tre build
sudo dnf install -y -q gcc gcc-c++ make git flex bison readline-devel zlib-devel \
     libicu-devel perl gettext-devel autoconf automake libtool python3 sysstat >/dev/null 2>&1 || true
echo "tuned: THP=$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
REMOTE
}

terminate() {
    local iid
    iid=$(cat "$IIDFILE" 2>/dev/null || true)
    [ -z "$iid" ] && { log "no instance id in $IIDFILE"; exit 1; }
    log "terminating $iid"
    aws ec2 terminate-instances --instance-ids "$iid" \
        --query 'TerminatingInstances[0].CurrentState.Name' --output text
}

case "${1:-}" in
    launch)     launch ;;
    setup-nvme) setup_nvme ;;
    tune)       tune_os ;;
    terminate)  terminate ;;
    *) echo "usage: $0 {launch|setup-nvme|tune|terminate}" >&2; exit 2 ;;
esac
