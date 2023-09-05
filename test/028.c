#include "stdbool.h"
#define INT_MAX 100000
#define V       10

int minDistance(int dist[], bool sptSet[]) {
    int min = INT_MAX, min_index = 0;
    for (int v = 0; v < V; v++)
        if (sptSet[v] == false && dist[v] <= min)
            min = dist[v], min_index = v;
    return min_index;
}

void dijkstra(int graph[V][V], int dist[V], int src) {
    bool sptSet[V];
    for (int i = 0; i < V; i++)
        dist[i] = INT_MAX, sptSet[i] = false;
    dist[src] = 0;

    for (int count = 0; count < V - 1; count++) {
        int u     = minDistance(dist, sptSet);
        sptSet[u] = true;
        for (int v = 0; v < V; v++)
            if (!sptSet[v] && graph[u][v] && dist[u] != INT_MAX && dist[u] + graph[u][v] < dist[v])
                dist[v] = dist[u] + graph[u][v];
    }
}

void init_graph(int graph[V][V]) {
    for (int i = 0; i < V; i++)
        for (int j = 0; j < V; j++)
            if (graph[i][j] < 0)
                graph[i][j] = INT_MAX;
}

int graph[V][V] = {
    {0, 9, 7, -1, 9, 7, -1, 5, -1, -1}, {9, 0, -1, -1, -1, 4, 5, -1, 4, -1}, {7, -1, 0, 2, 6, -1, 9, 2, 3, 5},
    {-1, -1, 2, 0, 8, 1, 9, 4, 5, 7},   {9, -1, 6, 8, 0, 1, 2, 8, 2, 1},     {7, 4, -1, 1, 1, 0, 9, 3, -1, -1},
    {-1, 5, 9, 9, 2, 9, 0, 2, 7, 3},    {5, -1, 2, 4, 8, 3, 2, 0, 5, 4},     {-1, 4, 3, 5, 2, -1, 7, 5, 0, -1},
    {-1, -1, 5, 7, 1, -1, 3, 4, -1, 0},
};
int answer[V] = {0, 9, 7, 8, 8, 7, 7, 5, 10, 9};
int dist[V];

int main() {
    init_graph(graph);
    dijkstra(graph, dist, 0);

    for (int i = 0; i < V; i++)
        if (dist[i] != answer[i])
            return 1;
    return 0;
}