import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

def generate_comparison_plots():
    phase123_csv_path = '../cplus_third/summary_stats.csv'
    phase4_csv_path = 'summary_stats_phase4.csv'

   
    try:
        df_prev = pd.read_csv(phase123_csv_path)
        df_curr = pd.read_csv(phase4_csv_path)
        df = pd.concat([df_prev, df_curr], ignore_index=True)
    except FileNotFoundError as e:
        print(f"File not found error: {e}")
        print("Ensure you are running this script directly from the cplus_fourth directory.")
        return

    
    metrics = ['mean_ns', 'p50_ns', 'p90_ns', 'p99_ns']
    labels = ['Mean', 'P50', 'P90', 'P99']
    
    
    target_phases = ['phase3_map_list', 'phase4_bitmask_pool']
    legend_labels = ['Phase 3 (Map/List)', 'Phase 4 (Bitmask/Pool)']
    colors = ['#FF9999', '#66B2FF']

    for workload in ['dense', 'sparse']:
        subset = df[df['workload'] == workload]
        if subset.empty:
            print(f"No data found for workload: {workload}")
            continue

        fig, ax = plt.subplots(figsize=(8, 5))
        x = np.arange(len(labels))
        width = 0.35

        rects_list = []
        
        
        for i, phase_key in enumerate(target_phases):
            phase_data = subset[subset['phase'] == phase_key]
            
            if not phase_data.empty:
                
                data = phase_data[metrics].values.flatten()
                
                
                offset = (i - 0.5) * width
                rects = ax.bar(x + offset, data, width, label=legend_labels[i], color=colors[i], edgecolor='black')
                rects_list.append(rects)

        ax.set_ylabel('Latency (ns)')
        ax.set_title(f'Latency Percentiles: Phase 3 vs Phase 4 ({workload.capitalize()} Workload)')
        ax.set_xticks(x)
        ax.set_xticklabels(labels)
        ax.legend()
        ax.grid(axis='y', linestyle='--', alpha=0.7)
        
       
        for rects in rects_list:
            for rect in rects:
                height = rect.get_height()
                
                ax.annotate(f'{height:.0f}',
                            xy=(rect.get_x() + rect.get_width() / 2, height),
                            xytext=(0, 3), 
                            textcoords="offset points",
                            ha='center', va='bottom')
                            
        plt.tight_layout()
        

        filename = f'comparison_phase3_vs_4_{workload}.png'
        plt.savefig(filename, dpi=300)
        print(f"Successfully generated and saved: {filename}")

if __name__ == "__main__":
    generate_comparison_plots()