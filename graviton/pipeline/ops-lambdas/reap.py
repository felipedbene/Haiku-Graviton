"""ReapBuilder: terminate the builder. Runs on BOTH the success and failure
paths of the state machine (the native build path had no reaper -- this is it).

Input : {instance_id}
Output: adds {reaped: instance_id}. Idempotent: terminating an already-gone
        instance is not an error we care about.
"""
import boto3


def handler(event, context):
    iid = event.get("instance_id")
    if iid:
        try:
            boto3.client("ec2").terminate_instances(InstanceIds=[iid])
        except Exception as e:  # already gone / race -> best effort
            event["reap_warning"] = str(e)
    event["reaped"] = iid
    return event
