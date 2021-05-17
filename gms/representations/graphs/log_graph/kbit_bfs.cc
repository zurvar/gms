// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#include <iostream>
#include <vector>

#include <gms/third_party/gapbs/benchmark.h>
#include <gms/third_party/gapbs/bitmap.h>
#include <gms/third_party/gapbs/builder.h>
#include <gms/third_party/gapbs/command_line.h>
#include "kbit_adjacency_array.h"
#include "kbit_adjacency_array_local.h"
#include <gms/third_party/gapbs/platform_atomics.h>
#include <gms/third_party/gapbs/pvector.h>
#include <gms/third_party/gapbs/sliding_queue.h>
#include <gms/third_party/gapbs/timer.h>
#include <omp.h>

/*
GAP Benchmark Suite
Kernel: Breadth-First Search (BFS)
Author: Scott Beamer

Will return parent array for a BFS traversal from a source vertex

This BFS implementation makes use of the Direction-Optimizing approach [1].
It uses the alpha and beta parameters to determine whether to switch search
directions. For representing the frontier, it uses a SlidingQueue for the
top-down approach and a Bitmap for the bottom-up approach. To reduce
false-sharing for the top-down approach, thread-local QueueBuffer's are used.

To save time computing the number of edges exiting the frontier, this
implementation precomputes the degrees in bulk at the beginning by storing
them in parent array as negative numbers. Thus the encoding of parent is:
  parent[x] < 0 implies x is unvisited and parent[x] = -out_degree(x)
  parent[x] >= 0 implies x been visited

[1] Scott Beamer, Krste Asanović, and David Patterson. "Direction-Optimizing
	Breadth-First Search." International Conference on High Performance
	Computing, Networking, Storage and Analysis (SC), Salt Lake City, Utah,
	November 2012.
*/


using namespace std;

int64_t BUStep(const My_Graph &g, pvector<NodeId> &parent, Bitmap &front,
			   Bitmap &next) {
	int64_t awake_count = 0;
	next.reset();

	#pragma omp parallel for reduction(+ : awake_count) schedule(dynamic, 1024)
	for (NodeId u=0; u < g.num_nodes(); u++) {
		if (parent[u] < 0) {
			ITERATE_NEIGHBOURHOOD(v, u,
				if (front.get_bit(v)) {
					parent[u] = v;
					awake_count++;
					next.set_bit(u);
					break;
				}
			)
		}
	}
  return awake_count;
}

int64_t TDStep(const My_Graph &g, pvector<NodeId> &parent,
			   SlidingQueue<NodeId> &queue) {
	int64_t scout_count = 0;

 	#pragma omp parallel
	{
	QueueBuffer<NodeId> lqueue(queue);

	#pragma omp for reduction(+ : scout_count)
	for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
		NodeId u = *q_iter;
     //  	for (NodeId v : g.in_neigh(u)) {
	 	ITERATE_NEIGHBOURHOOD(v, u,
			NodeId curr_val = parent[v];
			if (curr_val < 0) {
		  		if (compare_and_swap(parent[v], curr_val, u)) {
					lqueue.push_back(v);
					scout_count += -curr_val;
		  		}
			}
	  	)
	}
	lqueue.flush();
  }
  return scout_count;
}

void QueueToBitmap(const SlidingQueue<NodeId> &queue, Bitmap &bm) {
  #pragma omp parallel for
  for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
	NodeId u = *q_iter;
	bm.set_bit_atomic(u);
  }
}

void BitmapToQueue(const My_Graph &g, const Bitmap &bm,
				   SlidingQueue<NodeId> &queue) {
  #pragma omp parallel
  {
	QueueBuffer<NodeId> lqueue(queue);
	#pragma omp for
	for (NodeId n=0; n < g.num_nodes(); n++)
	  if (bm.get_bit(n))
		lqueue.push_back(n);
	lqueue.flush();
  }
  queue.slide_window();
}

pvector<NodeId> InitParent(const My_Graph &g) {
  pvector<NodeId> parent(g.num_nodes());
  #pragma omp parallel for
  for (NodeId n=0; n < g.num_nodes(); n++)
	parent[n] = g.out_degree(n) != 0 ? -g.out_degree(n) : -1;
  return parent;
}

pvector<NodeId> DOBFS(const My_Graph &g, NodeId source, int alpha = 15,
					  int beta = 18) {

	#if PRINT_INFO
		Timer t;
		PrintStep("Source", static_cast<int64_t>(source));
		t.Start();
	#endif
	pvector<NodeId> parent = InitParent(g);

	#if PRINT_INFO
		t.Stop();
		PrintStep("i", t.Seconds());
	#endif
	parent[source] = source;
	SlidingQueue<NodeId> queue(g.num_nodes());
	queue.push_back(source);
	queue.slide_window();
	Bitmap curr(g.num_nodes());
	curr.reset();
	Bitmap front(g.num_nodes());
	front.reset();
	int64_t edges_to_check = g.num_edges_directed();
	int64_t scout_count = g.out_degree(source);
	while (!queue.empty()) {
		if (scout_count > edges_to_check / alpha) {
			int64_t awake_count, old_awake_count;
			#if PRINT_INFO
				TIME_OP(t, QueueToBitmap(queue, front));
				PrintStep("e", t.Seconds());
			#else
				QueueToBitmap(queue, front);
			#endif
			awake_count = queue.size();
			queue.slide_window();
			do {
				#if PRINT_INFO
					t.Start();
				#endif
				old_awake_count = awake_count;
				awake_count = BUStep(g, parent, front, curr);
				front.swap(curr);
				#if PRINT_INFO
					t.Stop();
					PrintStep("bu", t.Seconds(), awake_count);
				#endif
			} while ((awake_count >= old_awake_count) ||
			   (awake_count > g.num_nodes() / beta));

			#if PRINT_INFO
				TIME_OP(t, BitmapToQueue(g, front, queue));
				PrintStep("c", t.Seconds());
			#else
				BitmapToQueue(g, front, queue);
			#endif
			scout_count = 1;
		} else {
			#if PRINT_INFO
	  			t.Start();
			#endif
			edges_to_check -= scout_count;
			scout_count = TDStep(g, parent, queue);
			queue.slide_window();
			#if PRINT_INFO
				t.Stop();
				PrintStep("td", t.Seconds(), queue.size());
			#endif
		}
	}
	return parent;
}

void PrintBFSStats(const My_Graph &g, const pvector<NodeId> &bfs_tree) {
	int64_t tree_size = 0;
	int64_t n_edges = 0;
	for (NodeId n : g.vertices()) {
		if (bfs_tree[n] >= 0) {
 			n_edges += g.out_degree(n);
 			tree_size++;
		}
	}
	cout << "BFS Tree has " << tree_size << " nodes and ";
	cout << n_edges << " edges" << endl;
}


// BFS verifier does a serial BFS from same source and asserts:
// - parent[source] = source
// - parent[v] = u  =>  depth[v] = depth[u] + 1 (except for source)
// - parent[v] = u  => there is edge from u to v
// - all vertices reachable from source have a parent
bool BFSVerifier(const My_Graph &g, NodeId source,
				 const pvector<NodeId> &parent) {
	pvector<int> depth(g.num_nodes(), -1);
	depth[source] = 0;
	vector<NodeId> to_visit;
	to_visit.reserve(g.num_nodes());
	to_visit.push_back(source);
	for (auto it = to_visit.begin(); it != to_visit.end(); it++) {
		NodeId u = *it;
	 	ITERATE_NEIGHBOURHOOD(v, u,
		  	if (depth[v] == -1) {
				depth[v] = depth[u] + 1;
				to_visit.push_back(v);
		  	}
		)
	}
	for (NodeId u : g.vertices()) {
		if ((depth[u] != -1) && (parent[u] != -1)) {
	  		if (u == source) {
				if (!((parent[u] == u) && (depth[u] == 0))) {
		  			cout << "Source wrong" << endl;
		  			return false;
				}
				continue;
	  		}
		  	bool parent_found = false;
			ITERATE_NEIGHBOURHOOD(v, u,
				if (v == parent[u]) {
			  		if (depth[v] != depth[u] - 1) {
						cout << "Wrong depths for " << u << " & " << v << endl;
						return false;
			  		}
			  		parent_found = true;
			  		break;
				}
			)
		  	if (!parent_found) {
				cout << "Couldn't find edge from " << parent[u] << " to " << u << endl;
				return false;
		  	}
		}
		else if (depth[u] != parent[u]) {
		 	cout << "Reachability mismatch" << endl;
			return false;
		}
	}
	return true;
}

int main(int argc, char* argv[]) {
  	CLApp cli(argc, argv, "breadth-first search");
  	if (!cli.ParseArgs()){
		return -1;
	}

	Builder b(cli);
	// Graph g = b.MakeGraph();
	// My_Graph graph = b.csrToKbit(b.MakeGraph());
	// My_Graph graph = b.csrToKbitLocal(b.MakeGraph());
	// My_Graph graph = b.csrToMyGraph(b.MakeGraph());
	My_Graph graph = b.make_graph_from_CSR();
	// My_Graph graph = b.make_kbit_graph();
	// cout << typeid(My_Graph).name() << endl;

	// My_Graph graph = b.make_graph();
	// for(int u = 0; u < g.num_nodes(); u++){
	// 	cout << "u: ";
	// 	for(NodeId v : g.out_neigh(u)){
	// 		cout << v << " ";
	// 	}
	// 	cout << endl;
	// }
	// cout << endl;
	// for(int u = 0; u < graph.num_nodes(); u++){
	// 	cout << "u: ";
	// 	for(NodeId v : graph.out_neigh(u)){
	// 		cout << v << " ";
	// 	}
	// 	cout << endl;
	// }
	// int64_t total = 0;
	// int64_t total_differences = 0;
	// for(int u = 0; u < graph.num_nodes(); u++){
	// 	if(graph.degree(u) == 0){
	// 		continue;
		// }
	// 	int largest_in_neighbourhood = 0;
	// 	int largest_difference = 0;
	// 	int last = 0;
	// 	for(NodeId v : graph.in_neigh(u)){
	// 		if(v > largest_in_neighbourhood){
	// 			largest_in_neighbourhood = v;
	// 		}
	// 		int diff = v-last;
	// 		if(diff > largest_difference){
	// 			largest_difference = diff;
	// 		}
	// 		last = v;
	// 	}
	// 	cout <<"largest valu " << largest_in_neighbourhood << endl;
	// 	cout <<"largest diff " << largest_difference << endl;
	// 	int a, b;
	// 	if(largest_in_neighbourhood == 0){
	// 		a = graph.degree(u)*1;
	// 		b = graph.degree(u)*1;
	// 	}
	// 	else{
	// 		a = graph.degree(u)*ceil(log2(largest_in_neighbourhood));
	// 		b = graph.degree(u)*ceil(log2(largest_difference));
	// 	}
	// 	cout << graph.degree(u) <<  " neighbours" << endl ;
	// 	cout <<"difference in bits here " << a-b << endl;
	// 	total += a;
	// 	total_differences += b;
	// }
	// cout <<"total bit difference " << total-total_differences << endl;
	// int64_t size_in_bytes = total % 8 == 0 ? total/8 + 7 : total/8 + 8;
	// cout << "space in bytes: " << size_in_bytes << endl;
	// size_in_bytes = total_differences % 8 == 0 ? total_differences/8 + 7 : total_differences/8 + 8;
	// cout << "space in bytes: " << size_in_bytes << endl;
	//
	// return 0;

	SourcePicker<My_Graph> sp(graph, cli.start_vertex());
	auto BFSBound = [&sp] (const My_Graph &graph) {
		return DOBFS(graph, sp.PickNext());
	};
	SourcePicker<My_Graph> vsp(graph, cli.start_vertex());

	auto VerifierBound = [&vsp] (const My_Graph &graph, const pvector<NodeId> &parent) {
		  return BFSVerifier(graph, vsp.PickNext(), parent);
	};
	BenchmarkKernelLegacy(cli, graph, BFSBound, PrintBFSStats, VerifierBound);
	return 0;
}
