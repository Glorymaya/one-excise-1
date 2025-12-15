#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-static-routing.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("WANExtensionWithRedundancy");

/**
 * @brief Utility function to disable a NetDevice's state.
 * This simulates a link failure by turning the interface 'off'.
 *
 * @param device Pointer to the NetDevice to disable.
 */
void
DisableLink(Ptr<NetDevice> device)
{
    // Setting the device's state to 'down'
    device->SetAttribute("Active", BooleanValue(false));
    NS_LOG_INFO("Link disabled for NetDevice: " << device->GetIfIndex());

    // NOTE: In NS-3 static routing, disabling the link device does NOT automatically
    // remove the route entry. The traffic will still be forwarded, but the packets
    // will be dropped at the lower layer because the device is down.
    // The backup path should take effect in the forwarder after the primary
    // route fails to send.
}

int
main(int argc, char* argv[])
{
    // Set up logging
    LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
    LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);
    LogComponentEnable("WANExtensionWithRedundancy", LOG_LEVEL_ALL);

    // 1. Create three nodes: n0 (HQ), n1 (Branch), n2 (DC)
    NodeContainer nodes;
    nodes.Create(3);
    Ptr<Node> n0 = nodes.Get(0); // HQ
    Ptr<Node> n1 = nodes.Get(1); // Branch
    Ptr<Node> n2 = nodes.Get(2); // DC

    // Configuration for all Point-to-Point links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("2ms"));

    // Install Internet Stack
    InternetStackHelper stack;
    stack.Install(nodes);

    // --- Q1: Topology Extension and Link Creation ---
    
    // Define IP address helpers for three distinct networks
    Ipv4AddressHelper addressHQBranch; // Network 1: HQ <-> Branch
    addressHQBranch.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4AddressHelper addressHQDC;     // Network 2: HQ <-> DC
    addressHQDC.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4AddressHelper addressBranchDC; // Network 3: Branch <-> DC
    addressBranchDC.SetBase("10.1.3.0", "255.255.255.0");

    // Link A: HQ (n0) <-> Branch (n1)
    NodeContainer linkHQBranchNodes(n0, n1);
    NetDeviceContainer linkHQBranchDevices = p2p.Install(linkHQBranchNodes);
    Ipv4InterfaceContainer interfacesHQBranch = addressHQBranch.Assign(linkHQBranchDevices);
    // n0: 10.1.1.1, n1: 10.1.1.2
    
    // Link B: HQ (n0) <-> DC (n2)
    NodeContainer linkHQDCNodes(n0, n2);
    NetDeviceContainer linkHQDCDevices = p2p.Install(linkHQDCNodes);
    Ipv4InterfaceContainer interfacesHQDC = addressHQDC.Assign(linkHQDCDevices);
    // n0: 10.1.2.1, n2: 10.1.2.2
    
    // Link C: Branch (n1) <-> DC (n2)
    NodeContainer linkBranchDCNodes(n1, n2);
    NetDeviceContainer linkBranchDCDevices = p2p.Install(linkBranchDCNodes);
    Ipv4InterfaceContainer interfacesBranchDC = addressBranchDC.Assign(linkBranchDCDevices);
    // n1: 10.1.3.1, n2: 10.1.3.2

    // Set all nodes as routers to enable IP forwarding
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Ipv4> ipv4Node = nodes.Get(i)->GetObject<Ipv4>();
        ipv4Node->SetAttribute("IpForward", BooleanValue(true));
    }

    // --- Q2: Static Routing Table Configuration (Primary/Backup) ---

    // Get static routing protocol helper
    Ipv4StaticRoutingHelper staticRoutingHelper;

    // --- Configuration on HQ (n0) ---
    Ptr<Ipv4StaticRouting> staticRoutingN0 = staticRoutingHelper.GetStaticRouting(n0->GetObject<Ipv4>());
    // HQ (n0) needs a route to DC (Network 3: 10.1.3.0/24)

    // Primary path to DC (10.1.3.0/24) is direct via Link B (n0's 10.1.2.1 interface)
    // The next hop is n2's interface on Link B: 10.1.2.2
    // n0's interface index for Link B (HQ-DC) is 2 (0=Lo, 1=HQ-Branch, 2=HQ-DC)
    staticRoutingN0->AddNetworkRouteTo(
        Ipv4Address("10.1.3.0"),            // Destination network
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.2.2"),            // Next hop (DC's direct IP on the HQ-DC link)
        2,                                  // Output interface index (HQ-DC Link)
        10                                  // Metric (Primary - lower is better)
    );

    // Backup path to DC (10.1.3.0/24) goes through Branch (n1) via Link A (n0's 10.1.1.1 interface)
    // The next hop is n1's interface on Link A: 10.1.1.2
    // n0's interface index for Link A (HQ-Branch) is 1 (0=Lo, 1=HQ-Branch, 2=HQ-DC)
    staticRoutingN0->AddNetworkRouteTo(
        Ipv4Address("10.1.3.0"),            // Destination network
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.1.2"),            // Next hop (Branch's IP on the HQ-Branch link)
        1,                                  // Output interface index (HQ-Branch Link)
        20                                  // Metric (Backup - higher is worse, so primary is preferred)
    );

    // --- Configuration on Branch (n1) ---
    Ptr<Ipv4StaticRouting> staticRoutingN1 = staticRoutingHelper.GetStaticRouting(n1->GetObject<Ipv4>());
    // n1 needs a symmetric path to HQ's network (10.1.2.0/24) and DC's network (10.1.2.0/24)
    // Route to DC's network (10.1.2.0/24) via Link C (n1's 10.1.3.1 interface)
    // Next hop: n2's interface on Link C: 10.1.3.2
    // n1's interface index for Link C is 2 (0=Lo, 1=HQ-Branch, 2=Branch-DC)
    staticRoutingN1->AddNetworkRouteTo(
        Ipv4Address("10.1.2.0"),            // Destination network (HQ-DC link)
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.3.2"),            // Next hop (DC's IP on the Branch-DC link)
        2                                   // Output interface index (Branch-DC Link)
    );
    // Route to HQ's network (10.1.2.0/24) via Link A (n1's 10.1.1.2 interface)
    // Next hop: n0's interface on Link A: 10.1.1.1 (Only needed for Branch to HQ traffic)
    // This route isn't strictly necessary for the HQ->DC traffic backup path, but ensures symmetry.
    staticRoutingN1->AddNetworkRouteTo(
        Ipv4Address("10.1.2.0"),            // Destination network
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.1.1"),            // Next hop (HQ's IP on the HQ-Branch link)
        1                                   // Output interface index (HQ-Branch Link)
    );

    // --- Configuration on DC (n2) ---
    Ptr<Ipv4StaticRouting> staticRoutingN2 = staticRoutingHelper.GetStaticRouting(n2->GetObject<Ipv4>());
    // n2 needs a route to HQ's network (10.1.1.0/24) for symmetric return traffic

    // Primary path to HQ (10.1.1.0/24) is direct via Link B (n2's 10.1.2.2 interface)
    // The next hop is n0's interface on Link B: 10.1.2.1
    // n2's interface index for Link B (HQ-DC) is 1 (0=Lo, 1=HQ-DC, 2=Branch-DC)
    staticRoutingN2->AddNetworkRouteTo(
        Ipv4Address("10.1.1.0"),            // Destination network
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.2.1"),            // Next hop (HQ's direct IP on the HQ-DC link)
        1,                                  // Output interface index (HQ-DC Link)
        10                                  // Metric (Primary - lower is better)
    );

    // Backup path to HQ (10.1.1.0/24) goes through Branch (n1) via Link C (n2's 10.1.3.2 interface)
    // The next hop is n1's interface on Link C: 10.1.3.1
    // n2's interface index for Link C (Branch-DC) is 2
    staticRoutingN2->AddNetworkRouteTo(
        Ipv4Address("10.1.1.0"),            // Destination network
        Ipv4Mask("255.255.255.0"),          // Network mask
        Ipv4Address("10.1.3.1"),            // Next hop (Branch's IP on the Branch-DC link)
        2,                                  // Output interface index (Branch-DC Link)
        20                                  // Metric (Backup)
    );
    
    // --- Q3: Path Failure Simulation ---
    
    // Get the NetDevice for the primary HQ-DC link on the HQ side (n0)
    Ptr<NetDevice> n0_HQDC_Device = linkHQDCDevices.Get(0);
    
    // Schedule the primary link failure event at t=4.0 seconds
    Simulator::Schedule(Seconds(4.0), &DisableLink, n0_HQDC_Device);
    
    // To ensure symmetric failure (for demonstration, also disable the DC side)
    Ptr<NetDevice> n2_HQDC_Device = linkHQDCDevices.Get(1);
    Simulator::Schedule(Seconds(4.0), &DisableLink, n2_HQDC_Device);

    // --- Application Setup: Traffic from HQ (n0) to DC (n2) ---

    // Server on DC (n2)
    uint16_t port = 9;
    UdpEchoServerHelper echoServer(port);
    ApplicationContainer serverApps = echoServer.Install(n2);
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(15.0));

    // Client on HQ (n0) targeting DC's IP on the Branch-DC link network (10.1.3.2)
    // The destination IP must be in the network we want to test the route to: 10.1.3.0/24
    Ipv4Address dc_address_on_branch_link = interfacesBranchDC.GetAddress(1); // 10.1.3.2
    
    UdpEchoClientHelper echoClient(dc_address_on_branch_link, port);
    echoClient.SetAttribute("MaxPackets", UintegerValue(10));
    echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
    echoClient.SetAttribute("PacketSize", UintegerValue(1024));

    ApplicationContainer clientApps = echoClient.Install(n0);
    clientApps.Start(Seconds(2.0)); // Start before failure
    clientApps.Stop(Seconds(15.0));

    // --- Visualization and Tracing ---
    
    // Set up mobility for a clear triangular layout in NetAnim
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);
    n0->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 10.0, 0.0));  // HQ (Left)
    n1->GetObject<MobilityModel>()->SetPosition(Vector(10.0, 0.0, 0.0));  // Branch (Bottom)
    n2->GetObject<MobilityModel>()->SetPosition(Vector(20.0, 10.0, 0.0)); // DC (Right)

    AnimationInterface anim("scratch/exercise1-redundant-wan.xml");
    anim.UpdateNodeDescription(n0, "HQ (n0)");
    anim.UpdateNodeDescription(n1, "Branch (n1)");
    anim.UpdateNodeDescription(n2, "DC (n2)");
    
    // Print routing tables at various times
    Ptr<OutputStreamWrapper> routingStream =
        Create<OutputStreamWrapper>("scratch/exercise1-redundant-wan.routes", std::ios::out);
    // Before failure
    staticRoutingHelper.PrintRoutingTableAllAt(Seconds(1.0), routingStream); 
    // After failure
    staticRoutingHelper.PrintRoutingTableAllAt(Seconds(5.0), routingStream); 

    // Enable PCAP tracing
    p2p.EnablePcapAll("scratch/exercise1-redundant-wan");

    // Run simulation
    Simulator::Stop(Seconds(16.0));
    Simulator::Run();
    Simulator::Destroy();

    std::cout << "\n=== Exercise 1 Simulation Complete ===\n";
    std::cout << "Check the .routes file to see the pre- and post-failure routing tables.\n";
    std::cout << "Check the .pcap files to verify traffic flow switch (HQ->Branch->DC).\n";

    return 0;
}