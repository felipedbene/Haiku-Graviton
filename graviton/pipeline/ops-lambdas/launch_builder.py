"""LaunchBuilder: start a native Graviton Haiku builder from the builder AMI.

Input : {ami_param?, instance_type?, disk_gib?, subnet_id?, sg_id?,
         instance_profile_arn?, spot?, on_demand_fallback?, exec_id?}  -- infra
         ids fall back to env (set by the stack) so the agent only ever passes
         {chain, target}.
Output: adds {instance_id, instance_type, market} to the event.

Resolves the builder image from SSM (/haiku-graviton/builder-ami-id by default),
launches into the hardened build VPC's private-egress subnet, tags it
Name=haiku-native-builder + debeos-build-exec=<exec_id> so the reaper (and IAM)
can scope to exactly this instance.

Root disk: a chain's space need varies by orders of magnitude (a small library vs
llvm), so the volume is sized per launch via `disk_gib` rather than baked into the
AMI. DeBeOS grows the BFS root to fill whatever volume it is given during boot
(loader-driven auto-grow), so the builder AMI can stay lean and each wave asks for
what it needs. Undersizing here is not a soft failure -- it surfaces as build
corruption that is really ENOSPC, so the default is deliberately generous.

Capacity (issue 838): one spot request for one type in one AZ is not a launch
plan. When that pool is dry RunInstances raises InsufficientInstanceCapacity, and
the chain used to go straight to ReapOnFailure -- the driver starts each chain
once, so the chain was lost for the run. The launcher now walks every builder
subnet (AZ) for the primary type, then repeats the walk for each equivalent
Graviton type in FALLBACK_INSTANCE_TYPES, all on spot. Only when every spot pool
is dry AND on-demand fallback is explicitly enabled (event `on_demand_fallback`,
else env ON_DEMAND_FALLBACK; off by default, because SOP section 2 budgets
builders as spot) does it repeat the walk on-demand, and it says so in the log
and in the output `market`. If nothing launches it raises
BuilderCapacityUnavailable, which the state machine retries with backoff before
giving up. A caller-pinned `instance_type` / `subnet_id` narrows the walk to it.
"""
import os
import random
import boto3
from botocore.config import Config
from botocore.exceptions import ClientError

AMI_PARAM = os.environ.get("BUILDER_AMI_PARAM", "/haiku-graviton/builder-ami-id")
DEFAULT_DISK_GIB = int(os.environ.get("BUILDER_DISK_GIB", "200"))

# "This type has no room in this AZ right now" -- move on to the next pool.
# Anything else (bad AMI, IAM, quota) is not a capacity problem and fails as before.
CAPACITY_ERRORS = ("InsufficientInstanceCapacity", "Unsupported")


class BuilderCapacityUnavailable(Exception):
    """No candidate pool could launch a builder. The SFN retries on this name."""


def _csv(value):
    return [s for s in (value or "").split(",") if s]


def _truthy(value):
    if isinstance(value, bool):
        return value
    return str(value or "").strip().lower() in ("1", "true", "yes", "on")


def _subnets(event):
    """Every builder subnet, shuffled so concurrent launches spread across AZs."""
    if event.get("subnet_id"):
        return [event["subnet_id"]]
    subnets = _csv(os.environ.get("SUBNET_IDS"))
    if not subnets:
        raise RuntimeError("no SUBNET_IDS configured")
    random.shuffle(subnets)
    return subnets


def _instance_types(event):
    """The primary type first, then the equivalent fallbacks, de-duplicated."""
    if event.get("instance_type"):
        return [event["instance_type"]]
    types = [os.environ.get("INSTANCE_TYPE", "c8g.2xlarge")]
    for t in _csv(os.environ.get("FALLBACK_INSTANCE_TYPES")):
        if t not in types:
            types.append(t)
    return types


def _root_device(ec2, ami):
    """The AMI's own root device name -- do NOT hardcode /dev/xvda.

    A block-device mapping whose DeviceName does not match the image's root device
    is silently ignored by RunInstances: the instance boots on the AMI's baked size
    and the requested volume never appears. That failure looks like a healthy
    launch, so resolve the name from the image instead of assuming it.
    """
    img = ec2.describe_images(ImageIds=[ami])["Images"][0]
    return img.get("RootDeviceName") or "/dev/xvda"


def _launch(ec2, spec, types, subnets, spot):
    """First pool with room wins; returns (instance, type, market) or None."""
    market = "spot" if spot else "on-demand"
    for itype in types:
        for subnet in subnets:
            attempt = dict(spec, InstanceType=itype, SubnetId=subnet)
            if spot:
                attempt["InstanceMarketOptions"] = {
                    "MarketType": "spot",
                    "SpotOptions": {"SpotInstanceType": "one-time"}}
            try:
                return ec2.run_instances(**attempt)["Instances"][0], itype, market
            except ClientError as e:
                code = e.response.get("Error", {}).get("Code", "")
                if code not in CAPACITY_ERRORS:
                    raise
                print("launch_builder: no %s capacity for %s in %s (%s), trying next"
                      % (market, itype, subnet, code))
    return None


def handler(event, context):
    # InsufficientInstanceCapacity is an HTTP 500, so botocore's default policy
    # re-asks the same dry pool several times (~16 s for the one pool in issue
    # 838's run). Moving to another pool is the useful retry, so cap the per-call
    # attempts and let the walk below do the rest inside the Lambda timeout.
    ec2 = boto3.client("ec2", config=Config(retries={"mode": "standard",
                                                     "total_max_attempts": 2}))
    ssm = boto3.client("ssm")
    ami = ssm.get_parameter(Name=event.get("ami_param", AMI_PARAM))["Parameter"]["Value"]
    exec_id = event.get("exec_id", context.aws_request_id)
    disk_gib = int(event.get("disk_gib") or DEFAULT_DISK_GIB)

    spec = dict(
        ImageId=ami,
        MinCount=1, MaxCount=1,
        SecurityGroupIds=[event.get("sg_id") or os.environ["SG_ID"]],
        IamInstanceProfile={"Arn": event.get("instance_profile_arn") or os.environ["INSTANCE_PROFILE_ARN"]},
        TagSpecifications=[{
            "ResourceType": "instance",
            "Tags": [
                {"Key": "Name", "Value": "haiku-native-builder"},
                {"Key": "debeos-build-exec", "Value": exec_id},
                {"Key": "Project", "Value": "haiku-graviton"},
                # SpringClean's daily shift-end sweep calls StopInstances on
                # every untagged instance in the account and reads no liveness
                # signal at all, so a wave builder hours into a port is stopped
                # exactly like an idle one. It took all three native arm64
                # builders down mid-build on 2026-10-16 (issue 761). The wave
                # terminates its own builders, so nothing here leaks; this only
                # prevents a stop. `auto-delete` is deliberately left unset, so
                # a builder the wave fails to reap is still reclaimed.
                {"Key": "auto-stop", "Value": "no"},
            ],
        }],
    )
    spec["BlockDeviceMappings"] = [{
        "DeviceName": _root_device(ec2, ami),
        "Ebs": {"VolumeSize": disk_gib, "VolumeType": "gp3",
                "DeleteOnTermination": True},
    }]
    types, subnets = _instance_types(event), _subnets(event)
    spot = bool(event.get("spot", True))
    got = _launch(ec2, spec, types, subnets, spot)
    if got is None and spot and _truthy(
            event.get("on_demand_fallback", os.environ.get("ON_DEMAND_FALLBACK"))):
        print("launch_builder: every spot pool is dry (%s x %s); ON-DEMAND FALLBACK "
              "is enabled, so this builder is billed on-demand"
              % (",".join(types), ",".join(subnets)))
        got = _launch(ec2, spec, types, subnets, False)
    if got is None:
        raise BuilderCapacityUnavailable(
            "no %s capacity for %s in any of %s"
            % ("spot" if spot else "on-demand", ",".join(types), ",".join(subnets)))
    inst, itype, market = got
    print("launch_builder: launched %s as %s (%s)" % (inst["InstanceId"], itype, market))
    event["instance_id"] = inst["InstanceId"]
    event["instance_type"] = itype
    event["market"] = market
    event["disk_gib"] = disk_gib
    return event
