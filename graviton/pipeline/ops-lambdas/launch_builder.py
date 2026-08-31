"""LaunchBuilder: start a native Graviton Haiku builder from the builder AMI.

Input : {ami_param?, instance_type?, disk_gib?, subnet_id?, sg_id?,
         instance_profile_arn?, spot?, exec_id?}  -- infra ids fall back to env
         (set by the stack) so the agent only ever passes {chain, target}.
Output: adds {instance_id} to the event.

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
"""
import os
import random
import boto3

AMI_PARAM = os.environ.get("BUILDER_AMI_PARAM", "/haiku-graviton/builder-ami-id")
DEFAULT_DISK_GIB = int(os.environ.get("BUILDER_DISK_GIB", "200"))


def _pick_subnet(event):
    """Spread spot across AZs: pick a subnet per instance from the builder set."""
    if event.get("subnet_id"):
        return event["subnet_id"]
    subnets = [s for s in os.environ.get("SUBNET_IDS", "").split(",") if s]
    if not subnets:
        raise RuntimeError("no SUBNET_IDS configured")
    return random.choice(subnets)


def _root_device(ec2, ami):
    """The AMI's own root device name -- do NOT hardcode /dev/xvda.

    A block-device mapping whose DeviceName does not match the image's root device
    is silently ignored by RunInstances: the instance boots on the AMI's baked size
    and the requested volume never appears. That failure looks like a healthy
    launch, so resolve the name from the image instead of assuming it.
    """
    img = ec2.describe_images(ImageIds=[ami])["Images"][0]
    return img.get("RootDeviceName") or "/dev/xvda"


def handler(event, context):
    ec2 = boto3.client("ec2")
    ssm = boto3.client("ssm")
    ami = ssm.get_parameter(Name=event.get("ami_param", AMI_PARAM))["Parameter"]["Value"]
    exec_id = event.get("exec_id", context.aws_request_id)
    disk_gib = int(event.get("disk_gib") or DEFAULT_DISK_GIB)

    spec = dict(
        ImageId=ami,
        InstanceType=event.get("instance_type", os.environ.get("INSTANCE_TYPE", "c8g.2xlarge")),
        MinCount=1, MaxCount=1,
        SubnetId=_pick_subnet(event),
        SecurityGroupIds=[event.get("sg_id") or os.environ["SG_ID"]],
        IamInstanceProfile={"Arn": event.get("instance_profile_arn") or os.environ["INSTANCE_PROFILE_ARN"]},
        TagSpecifications=[{
            "ResourceType": "instance",
            "Tags": [
                {"Key": "Name", "Value": "haiku-native-builder"},
                {"Key": "debeos-build-exec", "Value": exec_id},
                {"Key": "Project", "Value": "haiku-graviton"},
            ],
        }],
    )
    spec["BlockDeviceMappings"] = [{
        "DeviceName": _root_device(ec2, ami),
        "Ebs": {"VolumeSize": disk_gib, "VolumeType": "gp3",
                "DeleteOnTermination": True},
    }]
    if event.get("spot", True):
        spec["InstanceMarketOptions"] = {"MarketType": "spot",
                                         "SpotOptions": {"SpotInstanceType": "one-time"}}
    inst = ec2.run_instances(**spec)["Instances"][0]
    event["instance_id"] = inst["InstanceId"]
    event["disk_gib"] = disk_gib
    return event
