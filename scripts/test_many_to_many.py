import time
import boto3
import paramiko

NB_WORKER_NODES = 8
DPDK_INTERFACE_DESCRIPTION = "DPDK"
SSH_INTERFACE_DESCRIPTION = "SSH"
MY_IPV4 = "172.31.5.181"

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh_key = paramiko.RSAKey.from_private_key_file("/home/ec2-user/.ssh/caleb-dpdk.pem")

class NodeAddr:
    def __init__(self, ip, mac):
        self.ip = ip
        self.mac = mac

    @classmethod
    def from_interface(cls, interface):
        return cls(interface.private_ip_address, interface.mac_address)

    @classmethod
    def from_interfaces(cls, interfaces, description):
        for interface in interfaces:
            if interface.description == description:
                return cls.from_interface(interface)
        return None

resource = boto3.resource('ec2', 'us-west-2')
client = boto3.client('ec2', 'us-west-2')

instances = resource.create_instances(
        ImageId="ami-0ad2ea3502dec8f3c",
        MinCount=(NB_WORKER_NODES + 1),
        MaxCount=(NB_WORKER_NODES + 1),
        InstanceType="c5.4xlarge",
        KeyName="caleb-dpdk",
        Placement={"GroupName" : "dpdk"},
        NetworkInterfaces=[
            {"DeleteOnTermination" : True,
             "DeviceIndex" : 0,
             "SubnetId" : "subnet-015ad05c",
             "Groups" : ["sg-02d2b144ac4984d94"],
             "Description" : SSH_INTERFACE_DESCRIPTION
            },
            {"DeleteOnTermination" : True,
             "DeviceIndex" : 1,
             "SubnetId" : "subnet-015ad05c",
             "Groups" : ["sg-2bdaad17"],
             "Description" : DPDK_INTERFACE_DESCRIPTION
            }
            ]
        )

print("Waiting for instances to reach running state")
for instance in instances:
    instance.wait_until_running()

time.sleep(10) #sometimes the instances need longer to initialize

workers = instances[:NB_WORKER_NODES]
worker_dpdk_addrs = [NodeAddr.from_interfaces(worker.network_interfaces, DPDK_INTERFACE_DESCRIPTION) for worker in workers]
worker_ssh_addrs = [NodeAddr.from_interfaces(worker.network_interfaces, SSH_INTERFACE_DESCRIPTION) for worker in workers]
 
controller = instances[NB_WORKER_NODES]
controller_dpdk_addr = NodeAddr.from_interfaces(controller.network_interfaces, DPDK_INTERFACE_DESCRIPTION)
controller_ssh_addr = NodeAddr.from_interfaces(controller.network_interfaces, SSH_INTERFACE_DESCRIPTION)


for i, worker in enumerate(workers):
    dpdk_addrs = [worker_dpdk_addrs[i]] + [worker_dpdk_addrs[x] for x in range(NB_WORKER_NODES) if x != i]
    dpdk_addr_txt = [f"{addr.ip},{addr.mac}" for addr in dpdk_addrs]
    dpdk_addr_txt = "\\n".join(dpdk_addr_txt)

    worker_cmds = [
        f"GIT_SSH_COMMAND=\"ssh -o StrictHostKeyChecking=no -i /home/ec2-user/.ssh/caleb-dpdk.pem\" git clone ssh://ec2-user@{MY_IPV4}/home/ec2-user/dpdk-transport dpdk-transport",
        f"echo -e '{dpdk_addr_txt}' > addr.txt",
        "sudo bash dpdk-transport/scripts/setup.sh",
        "cd dpdk-transport; make RTE_SDK=/home/ec2-user/dpdk-stable-19.11.14 RTE_TARGET=x86_64-native-linuxapp-gcc",
        "nohup sudo dpdk-transport/tests/many-to-many/x86_64-native-linuxapp-gcc/app/many-to-many -- -f addr.txt -n 10000 > out.txt 2>&1 &"
        ]


    ssh.connect(hostname=worker_ssh_addrs[i].ip, port=22, username="ec2-user", pkey=ssh_key)

    print(f"Executing commands on worker {i}: {worker_ssh_addrs[i].ip}")
    for cmd in worker_cmds:
        print(cmd)
        stdin, stdout, stderr = ssh.exec_command(cmd)
        print(stdout.read().decode())

    ssh.close()

dpdk_addrs = [controller_dpdk_addr] + worker_dpdk_addrs
dpdk_addr_txt = [f"{addr.ip},{addr.mac}" for addr in dpdk_addrs]
dpdk_addr_txt = "\\n".join(dpdk_addr_txt)

controller_cmds = [
    f"GIT_SSH_COMMAND=\"ssh -o StrictHostKeyChecking=no -i /home/ec2-user/.ssh/caleb-dpdk.pem\" git clone ssh://ec2-user@{MY_IPV4}/home/ec2-user/dpdk-transport dpdk-transport",
    f"echo -e '{dpdk_addr_txt}' > addr.txt",
    "sudo bash dpdk-transport/scripts/setup.sh",
    "cd dpdk-transport; make RTE_SDK=/home/ec2-user/dpdk-stable-19.11.14 RTE_TARGET=x86_64-native-linuxapp-gcc",
    "sudo dpdk-transport/tests/many-to-many/x86_64-native-linuxapp-gcc/app/many-to-many -- -f addr.txt -c | tee out.txt"
    ]


ssh.connect(hostname=controller_ssh_addr.ip, port=22, username="ec2-user", pkey=ssh_key)

print(f"Executing commands on controller: {controller_ssh_addr.ip}")
for cmd in controller_cmds:
    print(cmd)
    stdin, stdout, stderr = ssh.exec_command(cmd)
    print(stdout.read().decode())

ssh.close()


client.stop_instances(InstanceIds=[x.instance_id for x in instances])