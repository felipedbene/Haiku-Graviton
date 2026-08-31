"""LaunchBuilder: start a native Graviton Haiku builder from the builder AMI.

Input : {ami_param?, instance_type?, subnet_id?, sg_id?, instance_profile_arn?,
         spot?, exec_id?}  -- infra ids fall back to env (set by the stack) so the
         agent only ever passes {chain, target}.
Output: adds {instance_id} to the event.

Resolves the builder image from SSM (/haiku-graviton/builder-ami-id by default),
launches into the hardened build VPC's private-egress subnet, tags it
Name=haiku-native-builder + debeos-build-exec=<exec_id> so the reaper (and IAM)
can scope to exactly this instance.
"""
import os
import random
import boto3

AMI_PARAM = os.environ.get("BUILDER_AMI_PARAM", "/haiku-graviton/builder-ami-id")


def _pick_subnet(event):
    """Spread spot across AZs: pick a subnet per instance from the builder set."""
    if event.get("subnet_id"):
        return event["subnet_id"]
    subnets = [s for s in os.environ.get("SUBNET_IDS", "").split(",") if s]
    if not subnets:
        raise RuntimeError("no SUBNET_IDS configured")
    return random.choice(subnets)


def handler(event, context):
    ec2 = boto3.client("ec2")
    ssm = boto3.client("ssm")
    ami = ssm.get_parameter(Name=event.get("ami_param", AMI_PARAM))["Parameter"]["Value"]
    exec_id = event.get("exec_id", context.aws_request_id)

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
    if event.get("spot", True):
        spec["InstanceMarketOptions"] = {"MarketType": "spot",
                                         "SpotOptions": {"SpotInstanceType": "one-time"}}
    inst = ec2.run_instances(**spec)["Instances"][0]
    event["instance_id"] = inst["InstanceId"]
    return event
