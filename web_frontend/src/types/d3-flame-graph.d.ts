declare module 'd3-flame-graph' {
  export interface FlamegraphData {
    name: string;
    value: number;
    children?: FlamegraphData[];
  }

  interface FlameGraph {
    width(w: number): FlameGraph;
    height(h: number): FlameGraph;
    cellHeight(h: number): FlameGraph;
    minFrameSize(s: number): FlameGraph;
    transitionDuration(d: number): FlameGraph;
    title(t: string): FlameGraph;
    selfValue(v: boolean): FlameGraph;
    transitionEase(e: any): FlameGraph;
    onClick(fn: (d: FlamegraphData) => void): FlameGraph;
    search(term: string): void;
    resetZoom(): void;
  }

  function flamegraph(): FlameGraph;
  export default flamegraph;
}
