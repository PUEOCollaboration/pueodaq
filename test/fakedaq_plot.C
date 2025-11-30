
R__LOAD_LIBRARY(build/libpueodaq.so);

#include "../src/pueodaq.h" 

void fakedaq_plot(int index = 0, bool pdf = false, const char * format = "/tmp/fakedaq_%05d.dat")
{
  gStyle->SetLineScalePS(1);

  FILE * f = fopen(Form(format,index),"r");
  pueo_daq_event_data_t d;
  int nb = fread(&d, sizeof(pueo_daq_event_data_t), 1, f);

  printf("Read %d bytes\n", nb);

  printf(PUEO_DAQ_EVENT_HEADER_FORMAT, PUEO_DAQ_EVENT_HEADER_VALUES(d.header));

  for (int isurf = 0; isurf < 28; isurf++)
  {
    TCanvas * c = new TCanvas(Form("c%d",isurf),Form("LINK %d, SURF %d", isurf/7, isurf % 7), 1800, 1000);
    c->Divide(4,2,0.001,0.001);
    for (int ichan = 0; ichan < 8; ichan++)
    {



      c->cd(ichan+1);
      TGraph * g = new TGraph(PUEODAQ_NSAMP);
      int idx = isurf*8+ichan;
      for (int i = 0; i < g->GetN(); i++)
      {
        g->SetPoint(i,i/3., d.waveform_data[idx][i]);
      }
      g->SetTitle(Form("RMS=%f;ns;adc", g->GetRMS(2)));
      g->GetYaxis()->SetTitleOffset(1.7);
      g->GetYaxis()->CenterTitle();
      g->GetXaxis()->CenterTitle();
      g->GetXaxis()->SetRangeUser(0,1024/3.);
      g->Draw("alp");

      gPad->SetRightMargin(0.05);
      gPad->SetLeftMargin(0.15);
      gPad->SetGridx();
      gPad->SetGridy();
    }
    if (pdf) c->SaveAs(Form("/tmp/fakedaq_%05d.pdf%s", index, isurf == 0 ? "(" : isurf == 27 ? ")" : ""));
    c->Show();
  }
  printf("\n");

}

